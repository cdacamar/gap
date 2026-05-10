#include "os.h"

#include <cassert>

#include <string_view>
#include <type_traits>

#include "arena.h"
#include "config.h"
#include "constants.h"
#include "enum-utils.h"
#include "gap-core.h"
#include "gap-strings.h"
#include "list-helpers.h"
#include "macros.h"
#include "renderer.h"
#include "thread-ctx.h"
#include "utf-8.h"
#include "util.h"
#include "vec.h"

#define NOMINMAX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <tchar.h>
#include <windowsx.h> // GET_X_LPARAM(), GET_Y_LPARAM()

// For exception filter.
#include <dbghelp.h>
#include <shlwapi.h>

#pragma comment(lib, "dwmapi")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "user32")
#pragma comment(lib, "kernel32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "comctl32")
// This is required for loading correct comctl32 dll file.
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace OS
{
    namespace
    {
        using namespace Constants;

        constexpr int internal_max_path = MAX_PATH * 2;

        constexpr DWORD pipe_buffer_size = KB(4);

        struct Win32Entity;

        struct IOPipeBuffer
        {
            char buf[pipe_buffer_size];
        };

        struct IOPipeEndpoint
        {
            HANDLE read_pipe;
            HANDLE event;
            OVERLAPPED ovr;
            Win32Entity* buf;
        };

        struct IOPipeData
        {
            int exit_code;
            int count;        // Note: This can change as endpoints in 'events' below are closed.
            HANDLE events[3];
            HANDLE process;
            HANDLE job;
            Win32Entity* stdout_ep;
            Win32Entity* stderr_ep;
        };

        struct CoreDirData
        {
            char core_dir[internal_max_path];
            uint64_t size;
        };

        struct DirIterData
        {
            Win32Entity* core_dir;
            HANDLE handle;
            WIN32_FIND_DATAW find_data;
            DirIterFlags flags;
        };

        struct ThreadData
        {
            ThreadEntryPointFunctionType func;
            void* data_p;
            HANDLE handle;
            DWORD tid;
        };

        struct Win32Entity
        {
            Win32Entity* next;
            Win32Entity* prev;
            union
            {
                // Pipes.
                IOPipeBuffer pipe_buffer;
                IOPipeEndpoint pipe_ep;
                IOPipeData pipe_data;

                // Dir iter.
                CoreDirData core_dir;
                DirIterData dir_iter_data;

                // Thread.
                ThreadData thread;

                // Mutex.
                CRITICAL_SECTION mutex;

                // Condition variable.
                CONDITION_VARIABLE cv;
            };
            uint64_t gen;
        };

        struct Win32EntityList
        {
            Win32Entity* first;
            Win32Entity* last;
            uint64_t count;
        };

        using ProcessEnv = String8List;

        // Cached function pointers.
        using PFGetDpiForWindow = UINT(WINAPI *)(HWND);

        struct Win32BackendData
        {
            HWND              wnd;
            SystemInfo        sys_info;
            WNDCLASSEXW       wc;
            LARGE_INTEGER     start_time;
            LARGE_INTEGER     frequency;
            DWORD             keyboard_code_page;
            DWORD             ui_thread_id;
            DWORD             pipe_id;
            uint32_t          high_surrogate;
            CursorStyle       current_cursor;
            HCURSOR           cursors[count_of<CursorStyle>];
            Hz                refresh_rate;
            ScreenDimensions  screen_size;
            ScreenDimensions  min_window_size;
            RenderCoreData*   render_data;
            ProcessEnv        environment;
            PFGetDpiForWindow win32_GetDpiForWindow;
            WINDOWPLACEMENT   win_place;
            Events*           caller_event_queue;
            CRITICAL_SECTION  win32_arena_mutex;
            Arena::Arena*     win32_arena;
            Arena::Arena*     win32_event_arena;
            Win32EntityList   pipe_data_lst;
            Win32EntityList   dir_iter_data_lst;
            Win32EntityList   thread_lst;
            Win32EntityList   mutex_lst;
            Win32EntityList   cond_var_lst;
            Win32Entity*      entity_free_list;
            uint64_t          entity_gen;
            bool              resizing;
        };

        Win32BackendData impl_data;

        static Win32BackendData* win32_data()
        {
            return &impl_data;
        }

        String16 convert_utf8_to_utf16(Arena::Arena* arena, String8 utf8)
        {
            // Find the size first.
            auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.str, static_cast<int>(utf8.size), nullptr, 0);
            String16 utf16 = str16_cstr_alloc(arena, size);
            MultiByteToWideChar(CP_UTF8, 0, utf8.str, static_cast<int>(utf8.size), utf16.str, static_cast<int>(size));
            return utf16;
        }

        String8 convert_utf16_to_utf8(Arena::Arena* arena, String16 utf16)
        {
            // Find the size first.
            auto size = WideCharToMultiByte(CP_UTF8, 0, utf16.str, static_cast<int>(utf16.size), nullptr, 0, nullptr, nullptr);
            String8 utf8 = str8_cstr_alloc(arena, size);
            WideCharToMultiByte(CP_UTF8, 0, utf16.str, static_cast<int>(utf16.size), utf8.str, static_cast<int>(size), nullptr, nullptr);
            return utf8;
        }

        // Borrowed from SDL: WIN_ConvertUTF16toUTF8.
        UTF8::Codepoint combine_surrogate_pairs(uint32_t high, uint32_t low)
        {
            constexpr uint32_t surrogate_offset = 0x10000U - (0xD800U << 10) - 0xDC00U;
            const UTF8::Codepoint codepoint = (high << 10) + low + surrogate_offset;
            return codepoint;
        }

        Win32Entity* push_entity(Win32BackendData* data, Win32EntityList* lst)
        {
            Win32Entity* node = nullptr;
            EnterCriticalSection(&data->win32_arena_mutex);
            if (data->entity_free_list != nullptr)
            {
                node = data->entity_free_list;
                SLLStackPop(data->entity_free_list);
                zero_bytes(node);
            }
            else
            {
                node = Arena::push_array<Win32Entity>(data->win32_arena, 1);
            }
            node->gen = data->entity_gen++;
            DLLPushBack(lst->first, lst->last, node);
            ++lst->count;
            LeaveCriticalSection(&data->win32_arena_mutex);
            return node;
        }

        void release_entity(Win32BackendData* data, Win32EntityList* lst, Win32Entity* e)
        {
            EnterCriticalSection(&data->win32_arena_mutex);
            DLLRemove(lst->first, lst->last, e);
            --lst->count;
            SLLStackPush(data->entity_free_list, e);
            LeaveCriticalSection(&data->win32_arena_mutex);
        }

        struct AllocPipeDataInput
        {
            HANDLE process;
            HANDLE job;
            HANDLE stdout_pipe;
            HANDLE stderr_pipe;
        };

        Win32Entity* alloc_pipe_data(Win32BackendData* data, AllocPipeDataInput in)
        {
            Win32Entity* pipe_data     = push_entity(data, &data->pipe_data_lst);
            Win32Entity* stdout_ep     = push_entity(data, &data->pipe_data_lst);
            Win32Entity* stdout_ep_buf = push_entity(data, &data->pipe_data_lst);
            Win32Entity* stderr_ep     = push_entity(data, &data->pipe_data_lst);
            Win32Entity* stderr_ep_buf = push_entity(data, &data->pipe_data_lst);

            pipe_data->pipe_data.job = in.job;
            pipe_data->pipe_data.process = in.process;

            // Endpoints.
            stdout_ep->pipe_ep.read_pipe = in.stdout_pipe;
            stdout_ep->pipe_ep.event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            stdout_ep->pipe_ep.buf = stdout_ep_buf;
            stderr_ep->pipe_ep.read_pipe = in.stderr_pipe;
            stderr_ep->pipe_ep.event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            stderr_ep->pipe_ep.buf = stderr_ep_buf;

            // Connect.
            pipe_data->pipe_data.stdout_ep = stdout_ep;
            pipe_data->pipe_data.stderr_ep = stderr_ep;

            pipe_data->pipe_data.count = 3;
            pipe_data->pipe_data.events[0] = pipe_data->pipe_data.process;
            pipe_data->pipe_data.events[1] = stdout_ep->pipe_ep.event;
            pipe_data->pipe_data.events[2] = stderr_ep->pipe_ep.event;
            pipe_data->pipe_data.exit_code = 0;
            return pipe_data;
        }

        void release_pipe_data(Win32BackendData* data, Win32Entity* e)
        {
            // Remove the events.
            CloseHandle(e->pipe_data.stdout_ep->pipe_ep.event);
            CloseHandle(e->pipe_data.stderr_ep->pipe_ep.event);
            // Disconnect all nodes.
            // Buffers.
            release_entity(data, &data->pipe_data_lst, e->pipe_data.stdout_ep->pipe_ep.buf);
            release_entity(data, &data->pipe_data_lst, e->pipe_data.stderr_ep->pipe_ep.buf);
            // Endpoints.
            release_entity(data, &data->pipe_data_lst, e->pipe_data.stdout_ep);
            release_entity(data, &data->pipe_data_lst, e->pipe_data.stderr_ep);
            // Core entity.
            release_entity(data, &data->pipe_data_lst, e);
        }

        Win32Entity* alloc_dir_iter_data(Win32BackendData* data)
        {
            Win32Entity* dir_buf  = push_entity(data, &data->dir_iter_data_lst);
            Win32Entity* dir_data = push_entity(data, &data->dir_iter_data_lst);
            dir_data->dir_iter_data.core_dir = dir_buf;
            dir_data->dir_iter_data.handle = INVALID_HANDLE_VALUE;
            return dir_data;
        }

        void release_dir_iter_data(Win32BackendData* data, Win32Entity* e)
        {
            if (e->dir_iter_data.handle != INVALID_HANDLE_VALUE)
            {
                FindClose(e->dir_iter_data.handle);
            }
            // Release the core dir.
            release_entity(data, &data->dir_iter_data_lst, e->dir_iter_data.core_dir);
            // Core entity.
            release_entity(data, &data->dir_iter_data_lst, e);
        }

        OSWindow os_window(HWND hwnd)
        {
            return OSWindow(reinterpret_cast<PrimitiveType<OSWindow>>(hwnd));
        }

        HWND win32_window(OSWindow wind)
        {
            return reinterpret_cast<HWND>(wind);
        }

        FileHandle os_file_handle(HANDLE h)
        {
            return FileHandle{ reinterpret_cast<PrimitiveType<FileHandle>>(h) };
        }

        HANDLE win32_file_handle(FileHandle h)
        {
            return reinterpret_cast<HANDLE>(h);
        }

        ProcessHandle os_process_handle(HANDLE h)
        {
            return ProcessHandle{ reinterpret_cast<PrimitiveType<ProcessHandle>>(h) };
        }

        HANDLE win32_process_handle(ProcessHandle h)
        {
            return reinterpret_cast<HANDLE>(h);
        }

        IOPipe os_pipe_handle(Win32Entity* p)
        {
            return IOPipe{ reinterpret_cast<PrimitiveType<IOPipe>>(p) };
        }

        Win32Entity* win32_pipe_handle(IOPipe h)
        {
            return reinterpret_cast<Win32Entity*>(h);
        }

        DirIter os_dir_iter(Win32Entity* d)
        {
            return DirIter{ reinterpret_cast<PrimitiveType<DirIter>>(d) };
        }

        Win32Entity* win32_dir_iter(DirIter h)
        {
            return reinterpret_cast<Win32Entity*>(h);
        }

        LibraryHandle os_library_handle(HMODULE mod)
        {
            return LibraryHandle{ reinterpret_cast<PrimitiveType<LibraryHandle>>(mod) };
        }

        HMODULE win32_library_handle(LibraryHandle lib)
        {
            return reinterpret_cast<HMODULE>(lib);
        }

        Event* push_event(EventSort sort, OSWindow window)
        {
            Win32BackendData* data = win32_data();
            Event* e = Arena::push_array<Event>(data->win32_event_arena, 1);
            e->sort = sort;
            e->window = window;
            SLLQueuePush(data->caller_event_queue->first, data->caller_event_queue->last, e);
            ++data->caller_event_queue->count;
            return e;
        }

        // Functions
        static void update_keyboard_codepage(Win32BackendData* data)
        {
            // Retrieve keyboard code page, required for handling of non-Unicode Windows.
            HKL keyboard_layout = ::GetKeyboardLayout(0);
            LCID keyboard_lcid = MAKELCID(HIWORD(keyboard_layout), SORT_DEFAULT);
            if (::GetLocaleInfoW(keyboard_lcid, (LOCALE_RETURN_NUMBER | LOCALE_IDEFAULTANSICODEPAGE), (LPWSTR)&data->keyboard_code_page, sizeof(data->keyboard_code_page)) == 0)
                data->keyboard_code_page = CP_ACP; // Fallback to default ANSI code page when fails.
        }

        void init_cursors(Win32BackendData* data)
        {
            for (auto x = CursorStyle{}; x != CursorStyle::Count; x = extend(x))
            {
                switch (x)
                {
                case CursorStyle::Default:
                    data->cursors[rep(x)] = ::LoadCursor(nullptr, IDC_ARROW);
                    break;
                case CursorStyle::IBeam:
                    data->cursors[rep(x)] = ::LoadCursor(nullptr, IDC_IBEAM);
                    break;
                case CursorStyle::Select:
                    data->cursors[rep(x)] = ::LoadCursor(nullptr, IDC_HAND);
                    break;
                case CursorStyle::UpDownArrow:
                    data->cursors[rep(x)] = ::LoadCursor(nullptr, IDC_SIZENS);
                    break;
                case CursorStyle::LeftRightArrow:
                    data->cursors[rep(x)] = ::LoadCursor(nullptr, IDC_SIZEWE);
                    break;
                case CursorStyle::SouthEastArrow:
                    data->cursors[rep(x)] = ::LoadCursor(nullptr, IDC_SIZENWSE);
                    break;
                case CursorStyle::SouthWestArrow:
                    data->cursors[rep(x)] = ::LoadCursor(nullptr, IDC_SIZENESW);
                    break;
                case CursorStyle::SizeAll:
                    data->cursors[rep(x)] = ::LoadCursor(nullptr, IDC_SIZEALL);
                    break;
                case CursorStyle::Count:
                    break;
                default:
                    assert(not "fix cursor styles");
                }
            }
        }

        bool init_win32_base(OS::Win32BackendData* bd)
        {
            LARGE_INTEGER perf_frequency;
            LARGE_INTEGER perf_counter;
            if (!::QueryPerformanceFrequency(&perf_frequency))
                return false;
            if (!::QueryPerformanceCounter(&perf_counter))
                return false;

            bd->frequency = perf_frequency;
            bd->start_time = perf_counter;
            bd->ui_thread_id = GetCurrentThreadId();

            // Setup the system info.
            {
                SYSTEM_INFO sysinfo{};
                GetSystemInfo(&sysinfo);
                bd->sys_info.processor_count = OS::ProcCount{ sysinfo.dwNumberOfProcessors };
                bd->sys_info.page_size = OS::PageSize{ sysinfo.dwPageSize };
                bd->sys_info.large_page_size = OS::PageSize{ GetLargePageMinimum() };
                bd->sys_info.allocation_granularity = OS::AllocGranularity{ sysinfo.dwAllocationGranularity };
            }

#ifdef BUILD_TRACK_ARENA
            Arena::init_tracker_arena();
#endif // BUILD_TRACK_ARENA

            // Setup thread context.
            {
                ::Thread::TLD* tl_ctx = ::Thread::tld_alloc();
                ::Thread::tld_select(tl_ctx);
            }

            // Setup dynamic alloc state.
            {
                bd->win32_arena = Arena::alloc(Arena::default_params);
            }

            // Computer name can be allocated here.
            {
                wchar_t comp_name[MAX_COMPUTERNAME_LENGTH + 1]{};
                DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
                if (GetComputerNameW(comp_name, &size))
                {
                    String8 s = OS::convert_utf16_to_utf8(bd->win32_arena, str16(comp_name, size));
                    bd->sys_info.machine_name = { s.str, s.size };
                }
            }

            // Finally, we can set up the entity mutex.
            {
                InitializeCriticalSection(&bd->win32_arena_mutex);
            }

#ifdef BUILD_TRACK_ARENA
            Arena::init_tracker_mutex();
#endif // BUILD_TRACK_ARENA
            return true;
        }

        bool init_win32_wnd(HWND hwnd)
        {
            PROF_SCOPE();
            Win32BackendData* bd = win32_data();
            bd->wnd = hwnd;
            bd->refresh_rate = recompute_monitor_refresh_rate();
            update_keyboard_codepage(bd);
            init_cursors(bd);
            set_cursor(CursorStyle::Default);

            return true;
        }

        // Allow compilation with old Windows SDK. MinGW doesn't have default _WIN32_WINNT/WINVER versions.
        #ifndef WM_MOUSEHWHEEL
        #define WM_MOUSEHWHEEL 0x020E
        #endif

        struct KeyTable
        {
            Key table[256];
        };

        constexpr KeyTable generate_key_table()
        {
            KeyTable table{};

            table.table[(unsigned int)'A'] = Key::A;
            table.table[(unsigned int)'B'] = Key::B;
            table.table[(unsigned int)'C'] = Key::C;
            table.table[(unsigned int)'D'] = Key::D;
            table.table[(unsigned int)'E'] = Key::E;
            table.table[(unsigned int)'F'] = Key::F;
            table.table[(unsigned int)'G'] = Key::G;
            table.table[(unsigned int)'H'] = Key::H;
            table.table[(unsigned int)'I'] = Key::I;
            table.table[(unsigned int)'J'] = Key::J;
            table.table[(unsigned int)'K'] = Key::K;
            table.table[(unsigned int)'L'] = Key::L;
            table.table[(unsigned int)'M'] = Key::M;
            table.table[(unsigned int)'N'] = Key::N;
            table.table[(unsigned int)'O'] = Key::O;
            table.table[(unsigned int)'P'] = Key::P;
            table.table[(unsigned int)'Q'] = Key::Q;
            table.table[(unsigned int)'R'] = Key::R;
            table.table[(unsigned int)'S'] = Key::S;
            table.table[(unsigned int)'T'] = Key::T;
            table.table[(unsigned int)'U'] = Key::U;
            table.table[(unsigned int)'V'] = Key::V;
            table.table[(unsigned int)'W'] = Key::W;
            table.table[(unsigned int)'X'] = Key::X;
            table.table[(unsigned int)'Y'] = Key::Y;
            table.table[(unsigned int)'Z'] = Key::Z;

            for (uint64_t i = '0', j = rep(Key::_0); i <= '9'; ++i, ++j)
            {
                table.table[i] = static_cast<Key>(j);
            }

            for (uint64_t i = VK_NUMPAD0, j = rep(Key::_0); i <= VK_NUMPAD9; ++i, ++j)
            {
                table.table[i] = static_cast<Key>(j);
            }

            for (uint64_t i = VK_F1, j = rep(Key::F1); i <= VK_F24; ++i, ++j)
            {
                table.table[i] = static_cast<Key>(j);
            }

            table.table[VK_SPACE]     = Key::Space;
            table.table[VK_OEM_3]     = Key::Tick;
            table.table[VK_OEM_MINUS] = Key::Minus;
            table.table[VK_OEM_PLUS]  = Key::Equal;
            table.table[VK_OEM_4]     = Key::LeftBracket;
            table.table[VK_OEM_6]     = Key::RightBracket;
            table.table[VK_OEM_1]     = Key::Semicolon;
            table.table[VK_OEM_7]     = Key::Quote;
            table.table[VK_OEM_COMMA] = Key::Comma;
            table.table[VK_OEM_PERIOD]= Key::Period;
            table.table[VK_OEM_2]     = Key::Slash;
            table.table[VK_OEM_5]     = Key::BackSlash;

            table.table[VK_TAB]       = Key::Tab;
            table.table[VK_PAUSE]     = Key::Pause;
            table.table[VK_ESCAPE]    = Key::Esc;

            table.table[VK_UP]        = Key::Up;
            table.table[VK_LEFT]      = Key::Left;
            table.table[VK_DOWN]      = Key::Down;
            table.table[VK_RIGHT]     = Key::Right;

            table.table[VK_BACK]      = Key::Backspace;
            table.table[VK_RETURN]    = Key::Return;

            table.table[VK_DELETE]    = Key::Delete;
            table.table[VK_INSERT]    = Key::Insert;
            table.table[VK_PRIOR]     = Key::PageUp;
            table.table[VK_NEXT]      = Key::PageDown;
            table.table[VK_HOME]      = Key::Home;
            table.table[VK_END]       = Key::End;

            table.table[VK_CAPITAL]   = Key::CapsLock;
            table.table[VK_NUMLOCK]   = Key::NumLock;
            table.table[VK_SCROLL]    = Key::ScrollLock;
            table.table[VK_APPS]      = Key::Menu;

            table.table[VK_CONTROL]   = Key::Ctrl;
            table.table[VK_LCONTROL]  = Key::Ctrl;
            table.table[VK_RCONTROL]  = Key::Ctrl;
            table.table[VK_SHIFT]     = Key::Shift;
            table.table[VK_LSHIFT]    = Key::Shift;
            table.table[VK_RSHIFT]    = Key::Shift;
            table.table[VK_MENU]      = Key::Alt;
            table.table[VK_LMENU]     = Key::Alt;
            table.table[VK_RMENU]     = Key::Alt;

            table.table[VK_DIVIDE]   = Key::NumSlash;
            table.table[VK_MULTIPLY] = Key::NumStar;
            table.table[VK_SUBTRACT] = Key::NumMinus;
            table.table[VK_ADD]      = Key::NumPlus;
            table.table[VK_DECIMAL]  = Key::NumPeriod;

            for (uint32_t i = 0; i < 10; ++i)
            {
                table.table[VK_NUMPAD0 + i] = static_cast<Key>(static_cast<uint64_t>(extend(Key::Num0, i)));
            }

            for (uint32_t i = 0xDF, j = 0; i < 0xFF; ++i, ++j)
            {
                table.table[i] = static_cast<Key>(static_cast<uint64_t>(extend(Key::Ex0, j)));
            }
            return table;
        }

        Key os_w32_os_key_from_vkey(WPARAM vkey)
        {
            static constexpr KeyTable table = generate_key_table();

            Key key = table.table[vkey & bitmask8];
            return key;
        }

        // Gap-specific event IDs.
        enum class CustomEvents : uint32_t
        {
            ThreadWakeup         = WM_APP + rep(EventSort::GapThreadWakeup),
        };

        constexpr EventSort convert(CustomEvents e)
        {
            switch (e)
            {
            case CustomEvents::ThreadWakeup:
                return EventSort::GapThreadWakeup;
            }
            assert(not "invalid event");
            return {};
        }

        constexpr CustomEvents convert(EventSort e)
        {
            switch (e)
            {
            case EventSort::GapThreadWakeup:
                return CustomEvents::ThreadWakeup;
            }
            assert(not "invalid event");
            return {};
        }

        COLORREF as_colorref(const Vec4f& color)
        {
            // Our hex is in the form: RRGGBBAA.
            // COLORREF is expecting:  00BBGGRR.
            // To perform the conversion we will first chop the alpha
            // then select the bits we need.
            auto hex = color_rgb(color);
            // Our color is now: 00RRGGBB.
            COLORREF result = RGB((hex & 0x00FF0000) >> 16,
                                    (hex & 0x0000FF00) >> 8,
                                    hex & 0x000000FF);
            return result;
        }

#ifdef WIN32_GFX
        LRESULT wind_proc_core(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
        {
            LRESULT result = 0;
            Win32BackendData* bd = win32_data();
            if (bd->win32_event_arena == nullptr)
            {
                switch (uMsg)
                {
                case WM_PAINT:
                    {
                        PAINTSTRUCT ps = {0};
                        HDC dc = BeginPaint(hwnd, &ps);
                        HBRUSH brush = CreateSolidBrush(as_colorref(Config::diff_colors().background));
                        FillRect(dc, &ps.rcPaint, brush);
                        DeleteObject(brush);
                        EndPaint(hwnd, &ps);
                    }
                    [[fallthrough]];
                case WM_ERASEBKGND:
                    result = TRUE;
                    break;
                default:
                    result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                    break;
                }
                return result;
            }
            auto* window = hwnd;
            auto window_handle = os_window(window);
            bool release = false;

            switch (uMsg)
            {
            default:
                {
                    result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                }
                break;
            case WM_ENTERSIZEMOVE:
                {
                    bd->resizing = true;
                }
                break;

            case WM_EXITSIZEMOVE:
                {
                    bd->resizing = false;
                }
                break;

            // We need this extra call in order to ensure that swap buffers is called after WM_PAINT to avoid random artifacts.
            case WM_SIZING:
                {
                    if (bd->render_data)
                    {
                        update_frame(bd->render_data);
                    }
                }
                break;

            case WM_SIZE:
            case WM_PAINT:
                {
                    PAINTSTRUCT ps = {0};
                    BeginPaint(hwnd, &ps);
                    if (bd->render_data and not window_minimized(window_handle))
                    {
                        update_frame(bd->render_data);
                    }
                    EndPaint(hwnd, &ps);
                    ShowWindow(window, SW_SHOW);
                }
                break;

            case WM_CLOSE:
                {
                    push_event(EventSort::Quit, window_handle);
                }
                break;

            case WM_LBUTTONUP:
            case WM_MBUTTONUP:
            case WM_RBUTTONUP:
                {
                    release = 1;
                }
                [[fallthrough]];
            case WM_LBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_RBUTTONDOWN:
                {
                    Event* event = push_event(release ? EventSort::Release : EventSort::Press, window_handle);
                    switch (uMsg)
                    {
                    case WM_LBUTTONUP:
                    case WM_LBUTTONDOWN:
                        {
                            event->key = Key::LeftMouseButton;
                        }
                        break;
                    case WM_MBUTTONUP:
                    case WM_MBUTTONDOWN:
                        {
                            event->key = Key::MiddleMouseButton;
                        }
                        break;
                    case WM_RBUTTONUP:
                    case WM_RBUTTONDOWN:
                        {
                            event->key = Key::RightMouseButton;
                        }
                        break;
                    }
                    event->pos.x = (float)GET_X_LPARAM(lParam);
                    event->pos.y = (float)GET_Y_LPARAM(lParam);
                    if (release)
                    {
                        ReleaseCapture();
                    }
                    else
                    {
                        SetCapture(hwnd);
                    }
                }
                break;

            case WM_MOUSEMOVE:
                {
                    Event* event = push_event(EventSort::MouseMove, window_handle);
                    event->pos.x = (float)GET_X_LPARAM(lParam);
                    event->pos.y = (float)GET_Y_LPARAM(lParam);
                }
                break;

            case WM_MOUSEWHEEL:
                {
                    int16_t wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    Event* event = push_event(EventSort::Scroll, window_handle);
                    POINT p;
                    p.x = GET_X_LPARAM(lParam);
                    p.y = GET_Y_LPARAM(lParam);
                    ScreenToClient(window, &p);
                    float wheel_delta_amount = static_cast<float>(wheel_delta) / WHEEL_DELTA;
                    event->pos.x = (float)p.x;
                    event->pos.y = (float)p.y;
                    event->wheel_delta = Vec2f{ 0.f, wheel_delta_amount };
                }
                break;

            case WM_MOUSEHWHEEL:
                {
                    int16_t wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    Event* event = push_event(EventSort::Scroll, window_handle);
                    POINT p;
                    p.x = GET_X_LPARAM(lParam);
                    p.y = GET_Y_LPARAM(lParam);
                    ScreenToClient(window, &p);
                    float wheel_delta_amount = static_cast<float>(wheel_delta) / WHEEL_DELTA;
                    event->pos.x = (float)p.x;
                    event->pos.y = (float)p.y;
                    event->wheel_delta = Vec2f{ wheel_delta_amount, 0.f };
                }
                break;

            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                {
                    if (wParam != VK_MENU && (wParam < VK_F1 || VK_F24 < wParam || wParam == VK_F4))
                    {
                        result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                    }
                }
                [[fallthrough]];
            case WM_KEYDOWN:
            case WM_KEYUP:
                {
                    bool was_down = (lParam & bit31);
                    bool is_down = !(lParam & bit32);
                    bool is_repeat = false;
                    if (!is_down)
                    {
                        release = true;
                    }
                    else if (was_down)
                    {
                        is_repeat = true;
                    }

                    bool right_sided = false;
                    if ((lParam & bit25) &&
                        (wParam == VK_CONTROL || wParam == VK_RCONTROL ||
                        wParam == VK_MENU || wParam == VK_RMENU ||
                        wParam == VK_SHIFT || wParam == VK_RSHIFT))
                    {
                        right_sided = true;
                    }

                    Event* event = push_event(release ? EventSort::Release : EventSort::Press, window_handle);
                    event->key = os_w32_os_key_from_vkey(wParam);
                    event->repeat_count = lParam & bitmask16;
                    event->is_repeat = is_repeat;
                    event->right_sided = right_sided;
                    if (event->key == Key::Alt && implies(event->modifiers, KeyMods::Alt))
                    {
                        event->modifiers = remove_flag(event->modifiers, KeyMods::Alt);
                    }
                    if (event->key == Key::Ctrl && implies(event->modifiers, KeyMods::Ctrl))
                    {
                        event->modifiers = remove_flag(event->modifiers, KeyMods::Ctrl);
                    }
                    if (event->key == Key::Shift && implies(event->modifiers, KeyMods::Shift))
                    {
                        event->modifiers = remove_flag(event->modifiers, KeyMods::Shift);
                    }
                }
                break;

            case WM_SYSCHAR:
                {
                    result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                }
                break;

            case WM_UNICHAR:
                // Directly insert utf32 events.
                {
                    uint32_t character = static_cast<uint32_t>(wParam);
                    if (character >= 32 && character != 127)
                    {
                        Event* event = push_event(EventSort::Text, window_handle);
                        if (lParam & bit29)
                        {
                            event->modifiers |= KeyMods::Alt;
                        }
                        event->character = character;
                    }
                }
                break;
            case WM_CHAR:
                {
                    uint32_t character = static_cast<uint32_t>(wParam);
                    if (IS_HIGH_SURROGATE(wParam))
                    {
                        bd->high_surrogate = static_cast<uint32_t>(wParam);
                    }
                    else if (IS_SURROGATE_PAIR(bd->high_surrogate, wParam))
                    {
                        auto cp = combine_surrogate_pairs(bd->high_surrogate, static_cast<uint32_t>(wParam));
                        Event* event = push_event(EventSort::Text, window_handle);
                        if (lParam & bit29)
                        {
                            event->modifiers |= KeyMods::Alt;
                        }
                        event->character = cp;
                    }
                    else
                    {
                        bd->high_surrogate = 0;
                        if (character >= 32 && character != 127)
                        {
                            Event* event = push_event(EventSort::Text, window_handle);
                            if (lParam & bit29)
                            {
                                event->modifiers |= KeyMods::Alt;
                            }
                            event->character = character;
                        }
                    }
                }
                break;

            case WM_KILLFOCUS:
                {
                    push_event(EventSort::FocusLost, window_handle);
                    ReleaseCapture();
                }
                break;

            case WM_SETCURSOR:
                {
                    // This is required to restore cursor when transitioning from e.g resize borders to client area.
                    if (LOWORD(lParam) == HTCLIENT)
                    {
                        set_cursor(bd->current_cursor);
                        return 1;
                    }
                    result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                }
                break;

            case WM_DPICHANGED:
                {
                    //float new_dpi = (float)(wParam & 0xffff);
                    // TODO: Set DPI somewhere.
                }
                break;

            case WM_DROPFILES:
                {
                    // Clear the current drag list.
                    DropFileList drop_list{};
                    HDROP drop = reinterpret_cast<HDROP>(wParam);
                    POINT drop_pt{};
                    DragQueryPoint(drop, &drop_pt);
                    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-dragqueryfilew#return-value
                    // The value of 0xFFFFFFFF gives us the drop count.
                    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                    Event* event = push_event(EventSort::FileDrop, window_handle);
                    event->pos = Vec2f{ static_cast<float>(drop_pt.x), static_cast<float>(drop_pt.y) };
                    auto scratch = Arena::scratch_begin({ &bd->win32_event_arena, 1 });
                    for (UINT i = 0; i < count; ++i)
                    {
                        // Note: On the documentation above, this will _not_ include the null terminator, however the
                        // API DragQueryFileW expects a buffer which has room for a null terminator.  Here, we're relying
                        // on the fact that allocating a cstr will, by default, allocate us a buffer big enough for the null
                        // terminator... which is already has.
                        UINT string_len = DragQueryFileW(drop, i, nullptr, 0);
                        String16 utf16 = str16_cstr_alloc(scratch.arena, string_len);
                        DragQueryFileW(drop, i, utf16.str, string_len + 1);
                        String8 str8_file = convert_utf16_to_utf8(bd->win32_event_arena, utf16);
                        String8Node* node = Arena::push_array_no_zero<String8Node>(bd->win32_event_arena, 1);
                        node->string = str8_file;
                        SLLQueuePush(drop_list.first, drop_list.last, node);
                        ++drop_list.count;
                    }
                    DragFinish(drop);
                    Arena::scratch_end(scratch);
                    bd->caller_event_queue->drop_files = drop_list;
                }
                break;

            case WM_NCPAINT:
                result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                break;
            case WM_DWMCOMPOSITIONCHANGED:
                result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                break;
            case WM_WINDOWPOSCHANGED:
                result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                break;
            case WM_SETICON:
            case WM_SETTEXT:
                result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                break;

            case WM_NCACTIVATE:
                result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                break;

            case WM_NCCALCSIZE:
                result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                break;

            case WM_NCHITTEST:
                result = DefWindowProcW(hwnd, uMsg, wParam, lParam);
                break;

            case WM_GETMINMAXINFO:
                LPMINMAXINFO mmi = reinterpret_cast<LPMINMAXINFO>(lParam);
                mmi->ptMinTrackSize.x = rep(bd->min_window_size.width);
                mmi->ptMinTrackSize.y = rep(bd->min_window_size.height);
                break;
            }
            return result;
        }

        LRESULT WINAPI wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            LRESULT result = wind_proc_core(hWnd, msg, wParam, lParam);
            if (result != 0)
                return result;

            Win32BackendData* data = win32_data();
            switch (msg)
            {
            case WM_SIZE:
                if (wParam != SIZE_MINIMIZED)
                {
                    int width = LOWORD(lParam);
                    int height = HIWORD(lParam);
                    data->screen_size = ScreenDimensions{ Width{ width }, Height{ height } };
                }
                return 0;
            case WM_SYSCOMMAND:
                if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
                    return 0;
                break;
            case WM_DESTROY:
                ::PostQuitMessage(0);
                return 0;
            }
            //return ::DefWindowProcW(hWnd, msg, wParam, lParam);
            return result;
        }
#endif // WIN32_GFX

        template <typename String>
        int cr_insertion_count(String buf)
        {
            using CharT = std::decay_t<decltype(buf.str[0])>;
            int cr_insert_count = 0;
            if (buf.size != 0)
            {
                CharT prev = '\0';
                for (size_t i = 0; i < buf.size; ++i)
                {
                    if (buf.str[i] == '\n' and prev != '\r')
                    {
                        ++cr_insert_count;
                    }
                    prev = buf.str[i];
                }
            }
            return cr_insert_count;
        }

        template <typename String>
        HGLOBAL copy_to_global_reconcile_crlf(String buf)
        {
            // We need to account for any missing CRLF.
            using CharT = std::decay_t<decltype(buf.str[0])>;
            int cr_insert_count = cr_insertion_count(buf);

            HGLOBAL glb_mem = GlobalAlloc(GMEM_MOVEABLE, (cr_insert_count + buf.size + 1) * sizeof(CharT));
            CharT* glb_wstr = static_cast<CharT*>(GlobalLock(glb_mem));
            CharT* base = glb_wstr;
            CharT prev = '\0';
            for (size_t i = 0; i < buf.size; ++i)
            {
                if (buf.str[i] == '\n' and prev != '\r')
                {
                    *glb_wstr = '\r';
                    ++glb_wstr;
                }
                prev = buf.str[i];
                *glb_wstr = buf.str[i];
                ++glb_wstr;
            }
            *glb_wstr = '\0';
            (void)base;
            GlobalUnlock(glb_mem);
            return glb_mem;
        }

        String8 generate_header_for_html(Arena::Arena* arena, String8 html)
        {
            int cr_insert_count = cr_insertion_count(html);
            size_t total_size = static_cast<size_t>(cr_insert_count) + html.size;

            size_t start_fragment = 0;
            size_t end_fragment = total_size;

            // Let's just assume there will be 11 padded 0's 10,000,000,000 (will there really be 10M line copy-pastes?).
            assert(end_fragment < 10'000'000'000);

            // Note: we leave off 'StartSelection' and 'EndSelection' because the fragment _is_ the selection.
            // We also add an extra newline so that the fragment can start immediately.
            constexpr String8View header_target = str8_literal(
R"(Version:1.0
StartHTML:00000000000
EndHTML:00000000000
StartFragment:00000000000
EndFragment:00000000000
)");

            constexpr String8View header_replacement = str8_literal(
R"(Version:1.0
StartHTML:%011jd
EndHTML:%011jd
StartFragment:%011jd
EndFragment:%011jd
)");

            // Account for any extra CRs inserted into the header itself.
            cr_insert_count = cr_insertion_count(header_replacement);

            start_fragment += header_target.size + static_cast<size_t>(cr_insert_count);
            end_fragment += header_target.size + static_cast<size_t>(cr_insert_count);

            char fmt_buf[header_target.size + 10];
            String8 str = fmt_string(fmt_buf,
                                        header_replacement.str,
                                        start_fragment,
                                        end_fragment,
                                        start_fragment,
                                        end_fragment);
            String8 result = str8_copy(arena, str);
            assert(result.size == header_target.size);
            return result;
        }

        String8 generate_html_with_header(Arena::Arena* arena, String8 html)
        {
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            String8 header = generate_header_for_html(scratch.arena, html);
            String8 result = str8_cat(arena, header, html);
            Arena::scratch_end(scratch);
            return result;
        }

        template <typename Char>
        bool drive_prefix(Char letter, Char sep)
        {
            // Logic pulled from '_Is_drive_prefix' in <filesystem>.
            // Note: we do not apply the unaligned load logic since we're passed both characters explicitly.
            // test if letter and sep points to a prefix of the form X:
            letter &= 0xFFDFu; // transform lowercase drive letters into uppercase ones
            return sep == ':' and ((letter - 'A') < 26);
        }

        bool drive_prefixed_with_slash_question(std::string_view sv)
        {
            // Logic pulled from '_Is_drive_prefix_with_slash_slash_question' in <filesystem>.
            return sv.length() >= 6 and sv.starts_with(R"(\\?\)") and drive_prefix(sv.at(4), sv.at(5));
        }

        bool drive_prefixed_with_slash_question(std::wstring_view sv)
        {
            // Logic pulled from '_Is_drive_prefix_with_slash_slash_question' in <filesystem>.
            return sv.length() >= 6 and sv.starts_with(LR"(\\?\)") and drive_prefix(sv.at(4), sv.at(5));
        }

        template <typename Char>
        bool path_separator(Char c)
        {
            return c == '\\' or c == '/';
        }

        uint64_t path_root_end(String8 path)
        {
            // Assume relative, beginning of path.
            if (path.size < 2)
                return 0;
            if (drive_prefix(path.str[0], path.str[1]))
                return 2;
            // Relative path.
            if (not path_separator(path.str[0]))
                return 0;
            // UNC-style path, e.g. \\foo.
            if (path_separator(path.str[1]))
                return 2;
            // Unix-style path start, e.g. /root/foo.
            return 1;
        }

        struct UTF16FileBuffer
        {
            wchar_t buf[internal_max_path];
        };

        void remove_filesystem_artifacts(UTF16FileBuffer* buf)
        {
            // The behavior of trimming the name below is supported through '_Canonical' in <filesystem>.
            std::wstring_view buf_view = buf->buf;
            if (drive_prefixed_with_slash_question(buf_view))
            {
                std::rotate(std::begin(buf->buf),
                            // The pattern '\\?\' takes 4 characters to get to the drive letter.
                            std::begin(buf->buf) + 4,
                            // Note: we add an extra +1 because we want to also rotate the null terminator.
                            std::begin(buf->buf) + buf_view.length() + 1);
                // This should be a regular ol' path now.
                assert(not drive_prefixed_with_slash_question(std::wstring_view{ buf->buf }));
            }
            // Let's just special-case the '\\?\UNC\' pattern.
            else if (buf_view.starts_with(LR"(\\?\UNC\)"))
            {
                // Note: because UNC paths need to look like: \\foo\bar
                // We want to trim up to the 'C' and then simply transform it into an extra '\' character.
                std::rotate(std::begin(buf->buf),
                            // The pattern '\\?\UNC\' is 6 characters up to the 'C', which we will change
                            // further below into a '\'.
                            std::begin(buf->buf) + 6,
                            // Note: we add an extra +1 because we want to also rotate the null terminator.
                            std::begin(buf->buf) + buf_view.length() + 1);
                assert((buf->buf)[0] == L'C');
                (buf->buf)[0] = L'\\';
            }
        }

        // Borrowed from RADDBG.
        uint32_t u32_from_u64_saturate(uint64_t x)
        {
            if (x > max_U32)
                return max_U32;
            return static_cast<uint32_t>(x);
        }

        HRESULT WINAPI exception_dlg_callback(HWND, UINT msg, WPARAM, LPARAM lparam, LONG_PTR)
        {
            if(msg == TDN_HYPERLINK_CLICKED)
            {
                ShellExecuteW(NULL, L"open", (LPWSTR)lparam, NULL, NULL, SW_SHOWNORMAL);
            }
            return S_OK;
        }

        // Borrowed from RADDBG.
        LONG WINAPI exception_filter(EXCEPTION_POINTERS* except_ptrs)
        {
            static volatile LONG first = 0;
            // If another thread comes through here, we'll only capture the first thread's dump info and terminate
            // shortly thereafter.
            if (InterlockedCompareExchange(&first, 1, 0) != 0)
            {
                for (;;)
                {
                    Sleep(1000);
                }
            }

            constexpr int buf_size = 4096;
            WCHAR buf[buf_size] = { };
            int buf_len = 0;

            DWORD except_code = except_ptrs->ExceptionRecord->ExceptionCode;
            buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L"A fatal exception (code 0x%x) occurred. The process is terminating.\n", except_code);

            // Load dbghelp dynamically just in case it is missing.
            HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
            if (dbghelp != nullptr)
            {
                DWORD (WINAPI *dbg_SymSetOptions)(DWORD SymOptions);
                BOOL (WINAPI *dbg_SymInitializeW)(HANDLE hProcess, PCWSTR UserSearchPath, BOOL fInvadeProcess);
                BOOL (WINAPI *dbg_StackWalk64)(DWORD MachineType, HANDLE hProcess, HANDLE hThread,
                                            LPSTACKFRAME64 StackFrame, PVOID ContextRecord, PREAD_PROCESS_MEMORY_ROUTINE64 ReadMemoryRoutine,
                                            PFUNCTION_TABLE_ACCESS_ROUTINE64 FunctionTableAccessRoutine, PGET_MODULE_BASE_ROUTINE64 GetModuleBaseRoutine,
                                            PTRANSLATE_ADDRESS_ROUTINE64 TranslateAddress);
                PVOID (WINAPI *dbg_SymFunctionTableAccess64)(HANDLE hProcess, DWORD64 AddrBase);
                DWORD64 (WINAPI *dbg_SymGetModuleBase64)(HANDLE hProcess, DWORD64 qwAddr);
                BOOL (WINAPI *dbg_SymFromAddrW)(HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement, PSYMBOL_INFOW Symbol);
                BOOL (WINAPI *dbg_SymGetLineFromAddrW64)(HANDLE hProcess, DWORD64 dwAddr, PDWORD pdwDisplacement, PIMAGEHLP_LINEW64 Line);
                BOOL (WINAPI *dbg_SymGetModuleInfoW64)(HANDLE hProcess, DWORD64 qwAddr, PIMAGEHLP_MODULEW64 ModuleInfo);

                *(FARPROC*)&dbg_SymSetOptions            = GetProcAddress(dbghelp, "SymSetOptions");
                *(FARPROC*)&dbg_SymInitializeW           = GetProcAddress(dbghelp, "SymInitializeW");
                *(FARPROC*)&dbg_StackWalk64              = GetProcAddress(dbghelp, "StackWalk64");
                *(FARPROC*)&dbg_SymFunctionTableAccess64 = GetProcAddress(dbghelp, "SymFunctionTableAccess64");
                *(FARPROC*)&dbg_SymGetModuleBase64       = GetProcAddress(dbghelp, "SymGetModuleBase64");
                *(FARPROC*)&dbg_SymFromAddrW             = GetProcAddress(dbghelp, "SymFromAddrW");
                *(FARPROC*)&dbg_SymGetLineFromAddrW64    = GetProcAddress(dbghelp, "SymGetLineFromAddrW64");
                *(FARPROC*)&dbg_SymGetModuleInfoW64      = GetProcAddress(dbghelp, "SymGetModuleInfoW64");
                if (dbg_SymSetOptions != nullptr
                    and dbg_SymInitializeW != nullptr
                    and dbg_StackWalk64 != nullptr
                    and dbg_SymFunctionTableAccess64 != nullptr
                    and dbg_SymGetModuleBase64 != nullptr
                    and dbg_SymFromAddrW != nullptr
                    and dbg_SymGetLineFromAddrW64 != nullptr
                    and dbg_SymGetModuleInfoW64 != nullptr)
                {
                    HANDLE process = GetCurrentProcess();
                    HANDLE thread = GetCurrentThread();
                    CONTEXT* context = except_ptrs->ContextRecord;

                    WCHAR mod_path[internal_max_path];
                    GetModuleFileNameW(nullptr, mod_path, internal_max_path);
                    PathRemoveFileSpecW(mod_path);

                    dbg_SymSetOptions(SYMOPT_EXACT_SYMBOLS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
                    if (dbg_SymInitializeW(process, mod_path, TRUE))
                    {
                        // Validate that the gap PDB is good.
                        bool valid_pdb = false;
                        {
                            IMAGEHLP_MODULEW64 mod{ };
                            mod.SizeOfStruct = sizeof(mod);
                            if (dbg_SymGetModuleInfoW64(process, reinterpret_cast<DWORD64>(exception_filter), &mod))
                            {
                                valid_pdb = mod.SymType == SymPdb;
                            }
                        }

                        if (not valid_pdb)
                        {
                            buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len,
                               L"\nThe PDB debug information file for this executable is not valid or was not found. " BUILD_TITLE " must be rebuilt.\n");
                        }
                        else
                        {
                            STACKFRAME64 frame = {0};
                            DWORD image_type;
#if defined(_M_AMD64)
                            image_type = IMAGE_FILE_MACHINE_AMD64;
                            frame.AddrPC.Offset = context->Rip;
                            frame.AddrPC.Mode = AddrModeFlat;
                            frame.AddrFrame.Offset = context->Rbp;
                            frame.AddrFrame.Mode = AddrModeFlat;
                            frame.AddrStack.Offset = context->Rsp;
                            frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64)
                            image_type = IMAGE_FILE_MACHINE_ARM64;
                            frame.AddrPC.Offset = context->Pc;
                            frame.AddrPC.Mode = AddrModeFlat;
                            frame.AddrFrame.Offset = context->Fp;
                            frame.AddrFrame.Mode = AddrModeFlat;
                            frame.AddrStack.Offset = context->Sp;
                            frame.AddrStack.Mode = AddrModeFlat;
#else
#error unsupported
#endif
                            for (uint32_t idx = 0; ; ++idx)
                            {
                                const uint32_t max_frames = 32;
                                if (idx == max_frames)
                                {
                                    buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L"...");
                                    break;
                                }

                                if (!dbg_StackWalk64(image_type, process, thread, &frame, context, 0, dbg_SymFunctionTableAccess64, dbg_SymGetModuleBase64, 0))
                                    break;
                                uint64_t address = frame.AddrPC.Offset;
                                if (address == 0)
                                    break;
                                if (idx == 0)
                                {
                                    buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len,
                                                        L"\nPress Ctrl+C to copy this text to clipboard, then create a new issue at\n"
                                                        L"<a href=\"%S\">%S</a>\n\n", BUILD_ISSUES_LINK, BUILD_ISSUES_LINK_HUMAN);
                                    buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L"Call stack:\n");
                                }
                                buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L"%u. [0x%I64x]", idx + 1, address);

                                struct
                                {
                                    SYMBOL_INFOW info;
                                    WCHAR name[MAX_SYM_NAME];
                                } symbol = {0};

                                symbol.info.SizeOfStruct = sizeof(symbol.info);
                                symbol.info.MaxNameLen = MAX_SYM_NAME;
                                DWORD64 displacement = 0;
                                if (dbg_SymFromAddrW(process, address, &displacement, &symbol.info))
                                {
                                    buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L" %s +%u", symbol.info.Name, (DWORD)displacement);
                                    IMAGEHLP_LINEW64 line = {0};
                                    line.SizeOfStruct = sizeof(line);
                                    DWORD line_displacement = 0;
                                    if (dbg_SymGetLineFromAddrW64(process, address, &line_displacement, &line))
                                    {
                                        buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L", %s line %u", PathFindFileNameW(line.FileName), line.LineNumber);
                                    }
                                }
                                else
                                {
                                    IMAGEHLP_MODULEW64 module = {0};
                                    module.SizeOfStruct = sizeof(module);
                                    if(dbg_SymGetModuleInfoW64(process, address, &module))
                                    {
                                        buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L" %s", module.ModuleName);
                                    }
                                }
                                buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L"\n");
                            }
                        }
                    }
                }
            }
            buf_len += wnsprintfW(buf + buf_len, buf_size - buf_len, L"\nVersion: %S %S", VERSION_STRING, HUMAN_BUILD_STRING);

            TASKDIALOGCONFIG dialog = {0};
            dialog.cbSize = sizeof(dialog);
            dialog.dwFlags = TDF_SIZE_TO_CONTENT | TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
            dialog.pszMainIcon = TD_ERROR_ICON;
            dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
            dialog.pszWindowTitle = L"Fatal Exception";
            dialog.pszContent = buf;
            dialog.pfCallback = &exception_dlg_callback;
            TaskDialogIndirect(&dialog, 0, 0, 0);

            ExitProcess(1);
        }

        constexpr uint64_t compose_64_bit(uint32_t a, uint32_t b)
        {
            return (((uint64_t)a) << 32) | ((uint64_t)b);
        }

        void fill_date_time_from_system_time(DateTime* out, const SYSTEMTIME& in)
        {
            out->year    = in.wYear;
            out->mon     = in.wMonth - 1;
            out->wday    = in.wDayOfWeek;
            out->day     = in.wDay;
            out->hour    = in.wHour;
            out->min     = in.wMinute;
            out->sec     = in.wSecond;
            out->msec    = in.wMilliseconds;
        }

        void fill_system_time_from_date_time(SYSTEMTIME* out, const DateTime& in)
        {
            out->wYear         = static_cast<WORD>(in.year);
            out->wMonth        = static_cast<WORD>(in.mon + 1);
            out->wDay          = in.day;
            out->wHour         = in.hour;
            out->wMinute       = in.min;
            out->wSecond       = in.sec;
            out->wMilliseconds = in.msec;
        }

        DenseTime dense_time_from_file_time(const FILETIME& in)
        {
            SYSTEMTIME systime = {0};
            FileTimeToSystemTime(&in, &systime);
            DateTime date_time{ };
            fill_date_time_from_system_time(&date_time, systime);
            return dense_time_from_date_time(date_time);
        }

        FileProperty file_property_from_attrs(DWORD attrs)
        {
            FileProperty props = FileProperty::None;
            if(attrs & FILE_ATTRIBUTE_DIRECTORY)
            {
                props |= FileProperty::Directory;
            }
            return props;
        }

        struct CreateProcessParams
        {
            String16 wargs;
            String16 wenv;
            String16 wwd;
            WCHAR* env_ptr;
            DWORD create_flags;
        };

        void create_process_params(Arena::Arena* arena, CreateProcessParams* params, const ProcessLaunchParams& in)
        {
            // Create the full command line.
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessa#security-remarks
            // This indicates that it's always a better idea to quote all of our arguments.
            {
                // Do not introduce quotes despite the above since we will be targeting the command line.
                if (implies(in.flags, LaunchFlags::CLIProcess))
                {
                    // Account for separators (which are " at the beginning/end and " " in between each argument).
                    JoinStringsInput join_in{
                        .strings = in.cmd_line,
                        // Additionally, prepend the buffer with "/c" to execute the command.
                        .start_sep = str8_mut(str8_literal("/c ")),
                        .sep = str8_mut(str8_literal(" ")),
                        .end_sep = str8_mut(str8_literal(""))
                    };
                    String8 join_result = join_strings(scratch.arena, join_in);
                    params->wargs = convert_utf8_to_utf16(arena, join_result);
                }
                else
                {
                    // Account for separators (which are " at the beginning/end and " " in between each argument).
                    JoinStringsInput join_in{
                        .strings = in.cmd_line,
                        .start_sep = str8_mut(str8_literal("\"")),
                        .sep = str8_mut(str8_literal("\" \"")),
                        .end_sep = str8_mut(str8_literal("\""))
                    };
                    String8 join_result = join_strings(scratch.arena, join_in);
                    params->wargs = convert_utf8_to_utf16(arena, join_result);
                }
            }

            // Env.
            params->env_ptr = nullptr;
            {
                // Environment variables want to be a series of 'key1 = value1\0key2 = value2\0\0' strings.
                // If we're asked to inherit the environment and the input actually has an environment to store,
                // we will union the current process environment with the existing one.
                if (implies(in.flags, LaunchFlags::InheritEnv))
                {
                    if (in.env.node_count != 0)
                    {
                        Win32BackendData* data = win32_data();
                        // Combine both lists.
                        String8List env_lst{};
                        for (String8 str : in.env)
                        {
                            str8_list_push(scratch.arena, &env_lst, str);
                        }

                        for (String8 str : data->environment)
                        {
                            str8_list_push(scratch.arena, &env_lst, str);
                        }

                        JoinStringsInput join_in{
                            .strings = env_lst,
                            .start_sep = str8_empty,
                            .sep = str8_mut(str8_literal("\0")),
                            .end_sep = str8_mut(str8_literal("\0"))
                        };
                        String8 join_result = join_strings(scratch.arena, join_in);
                        params->wenv = convert_utf8_to_utf16(arena, join_result);
                        params->env_ptr = params->wenv.str;
                    }
                    else
                    {
                        params->env_ptr = nullptr;
                    }
                }
                else
                {
                    // Make the environment empty.
                    params->wenv = str16_cstr_alloc(arena, 0);
                    params->env_ptr = params->wenv.str;
                }
            }

            params->wwd = convert_utf8_to_utf16(arena, in.wd);

            params->create_flags = CREATE_UNICODE_ENVIRONMENT;
            if (implies(in.flags, LaunchFlags::Consoleless))
            {
                params->create_flags |= CREATE_NO_WINDOW;
            }

            if (implies(in.flags, LaunchFlags::Suspend))
            {
                params->create_flags |= CREATE_SUSPENDED;
            }
            Arena::scratch_end(scratch);
        }

        struct PipePair
        {
            HANDLE read;
            HANDLE write;
        };

        bool valid_pipe(PipePair pipe)
        {
            return pipe.read != INVALID_HANDLE_VALUE
                and pipe.write != INVALID_HANDLE_VALUE;
        }

        PipePair create_named_pipes()
        {
            Win32BackendData* data = win32_data();
            SECURITY_ATTRIBUTES attrs{};
            attrs.nLength = sizeof attrs;
            attrs.bInheritHandle = TRUE;

            WCHAR name[MAX_PATH];
            wnsprintfW(name, MAX_PATH, L"\\\\.\\Pipe\\GapNamedPipe.%08x.%08x", data->ui_thread_id, data->pipe_id++);

            PipePair ret{};
            ret.read = CreateNamedPipeW(name,
                                        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                        PIPE_TYPE_BYTE | PIPE_WAIT,
                                        1,
                                        pipe_buffer_size,
                                        pipe_buffer_size,
                                        0,
                                        &attrs);
            if (ret.read == INVALID_HANDLE_VALUE)
                return ret;

            ret.write = CreateFileW(name,
                                    GENERIC_WRITE,
                                    0,
                                    &attrs,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                    nullptr);
            if (ret.write == INVALID_HANDLE_VALUE)
            {
                CloseHandle(ret.read);
                ret.read = INVALID_HANDLE_VALUE;
            }
            return ret;
        }

        void close_pipes(PipePair pipe)
        {
            CloseHandle(pipe.read);
            CloseHandle(pipe.write);
        }

        // Returns true if a write to the buffer happened.
        bool read_pipe_endpoint(Arena::Arena* arena, String8* out_buf, IOPipeData* data, IOPipeEndpoint* ep, DWORD idx)
        {
            bool schedule_next = true;
            bool buf_written = false;
            // This could be the first wait, in which case the handle overlapped value will be null.
            if (ep->ovr.hEvent != nullptr)
            {
                DWORD read;
                if (GetOverlappedResult(ep->read_pipe, &ep->ovr, &read, FALSE))
                {
                    *out_buf = str8_copy(arena, str8(ep->buf->pipe_buffer.buf, read));
                    ep->ovr = { };
                    buf_written = true;
                }
                else
                {
                    // Swap this with a valid element.
                    data->events[idx] = data->events[data->count - 1];
                    --data->count;

                    // Close the read.
                    CloseHandle(ep->read_pipe);
                    ep->read_pipe = INVALID_HANDLE_VALUE;
                    // The event will be closed on deallocation.
                    schedule_next = false;
                }
            }

            if (schedule_next)
            {
                ep->ovr.hEvent = ep->event;
                if (not ReadFile(ep->read_pipe, ep->buf->pipe_buffer.buf, pipe_buffer_size, nullptr, &ep->ovr))
                {
                    DWORD err = GetLastError();
                    if (err == ERROR_BROKEN_PIPE)
                    {
                        // Swap this with a valid element.
                        data->events[idx] = data->events[data->count - 1];
                        --data->count;

                        // Close the read.
                        CloseHandle(ep->read_pipe);
                        ep->read_pipe = INVALID_HANDLE_VALUE;
                        // The event will be closed on deallocation.
                    }
                }
            }
            return buf_written;
        }

        void populate_file_props_from_find_data(FileProperties* props, const WIN32_FIND_DATAW& find_data)
        {
            props->size = FileLength{ compose_64_bit(find_data.nFileSizeHigh, find_data.nFileSizeLow) };
            props->created = dense_time_from_file_time(find_data.ftCreationTime);
            props->modified = dense_time_from_file_time(find_data.ftLastWriteTime);
            props->props = file_property_from_attrs(find_data.dwFileAttributes);
        }

        Mutex os_mutex(Win32Entity* e)
        {
            return Mutex{ reinterpret_cast<PrimitiveType<Mutex>>(e) };
        }

        Win32Entity* win32_mutex(Mutex m)
        {
            return reinterpret_cast<Win32Entity*>(m);
        }

        Win32Entity* win32_mutex_alloc(Win32BackendData* data)
        {
            Win32Entity* mutex = push_entity(data, &data->mutex_lst);
            InitializeCriticalSection(&mutex->mutex);
            return mutex;
        }

        void release_win32_mutex(Win32BackendData* data, Mutex m)
        {
            release_entity(data, &data->mutex_lst, win32_mutex(m));
        }

        ConditionVariable os_condition_var(Win32Entity* e)
        {
            return ConditionVariable{ reinterpret_cast<PrimitiveType<ConditionVariable>>(e) };
        }

        Win32Entity* win32_condition_var(ConditionVariable m)
        {
            return reinterpret_cast<Win32Entity*>(m);
        }

        Win32Entity* win32_condition_var_alloc(Win32BackendData* data)
        {
            Win32Entity* cv = push_entity(data, &data->cond_var_lst);
            InitializeConditionVariable(&cv->cv);
            return cv;
        }

        void release_win32_condition_var(Win32BackendData* data, ConditionVariable cv)
        {
            release_entity(data, &data->cond_var_lst, win32_condition_var(cv));
        }

        uint32_t win32_sleep_ms_from_end_us(MicroSec end_us)
        {
            uint32_t sleep_ms = 0;
            if (end_us == MicroSec::Infinite)
            {
                sleep_ms = INFINITE;
            }
            else
            {
                MicroSec cur = now_microseconds();
                if (cur < end_us)
                {
                    uint64_t sleep_us = rep(end_us) - rep(cur);
                    // We add 999 so we sleep for at least 1ms if there were any microsecond fractions < 1000.
                    sleep_ms = static_cast<uint32_t>((sleep_us + 999) / Thousand(1));
                }
            }
            return sleep_ms;
        }

        Thread os_thread(Win32Entity* e)
        {
            return Thread{ reinterpret_cast<PrimitiveType<Thread>>(e) };
        }

        Win32Entity* win32_thread(Thread t)
        {
            return reinterpret_cast<Win32Entity*>(t);
        }

        DWORD win32_thread_entry_point(void* ptr)
        {
            Win32Entity* e = static_cast<Win32Entity*>(ptr);
            thread_entry_bridge(e->thread.func, e->thread.data_p);
            return 0;
        }

        Win32Entity* win32_alloc_thread(Win32BackendData* data, ThreadEntryPointFunctionType func, void* data_p)
        {
            Win32Entity* thread = push_entity(data, &data->thread_lst);
            thread->thread.func = func;
            thread->thread.data_p = data_p;
            thread->thread.handle = CreateThread(nullptr,
                                                    0, // Default size for exe.
                                                    win32_thread_entry_point,
                                                    thread,
                                                    0,
                                                    &thread->thread.tid);
            return thread;
        }

        void win32_release_thread(Win32BackendData* data, Thread thread)
        {
            release_entity(data, &data->thread_lst, win32_thread(thread));
        }
    } // namespace [anon]

    // --- START GAP API ---
    // Error info.
    ENABLE_UNHANDLED_CASE_WARNING()
    String8 error_text(Error e)
    {
        switch (e)
        {
        case Error::ClipboardTextUnavailable:
            return str8_mut(str8_literal("Clipboard text unavailable or type is not text"));
        case Error::UnableToOpenClipboard:
            return str8_mut(str8_literal("Unable to open clipboard"));
        case Error::InvalidClipboardData:
            return str8_mut(str8_literal("Invalid clipboard data type"));
        case Error::EmptyClipboardFailed:
            return str8_mut(str8_literal("Unable to empty clipboard"));
        case Error::BufferTruncated:
            return str8_mut(str8_literal("Buffer truncated"));
        case Error::CouldNotLocatePrefPath:
            return str8_mut(str8_literal("Could not locate pref path"));
        case Error::CreateDirectoryFailed:
            return str8_mut(str8_literal("Failed to create directory"));
        case Error::FailedToApplyWindowCaptionColor:
            return str8_mut(str8_literal("Failed to apply window caption color"));
        case Error::FailedToApplyWindowFontColor:
            return str8_mut(str8_literal("Failed to apply window font color"));
        case Error::CanonicalizeFailedToOpenFile:
            return str8_mut(str8_literal("Failed to open file for canonical path"));
        case Error::CanonicalizeFailedToGetPathname:
            return str8_mut(str8_literal("Failed to get invoke GetFinalPathNameByHandleW"));
        case Error::None:
        case Error::Count:
            break;
        }
        assert(not "Unhandled case");
        return str8_empty;
    }
    DISABLE_UNHANDLED_CASE_WARNING()

    OSError last_os_error_code()
    {
        return OSError{ GetLastError() };
    }

    String8 format_error(Arena::Arena* arena, OSError err)
    {
        String8 result{};
        DWORD win32_err = rep(err);
        LPWSTR wbuf = nullptr;
        if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                            nullptr,
                            win32_err,
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                            reinterpret_cast<LPWSTR>(&wbuf),
                            0,
                            nullptr))
        {
            result = convert_utf16_to_utf8(arena, str16_cstr(wbuf));
            LocalFree(wbuf);
        }
        // API failed.
        else
        {
            char fmt_buf[100];
            String8 str = fmt_string(fmt_buf, "No format for GetLastError %08x,%u", rep(err), rep(err));
            result = str8_copy(arena, str);
        }
        return result;
    }

    // Windowing.
#ifdef WIN32_GFX
    OSWindow init_window(Vec4i wind_rect, String8 title)
    {
        PROF_BEGIN(wnd_core_ctx, "WIN32 - Core window creation");
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 utf16_title = convert_utf8_to_utf16(scratch.arena, title);
        Win32BackendData* data = win32_data();
        {
            bool dpi_set = false;
            // Windows 10 1703
            HMODULE user32 = LoadLibraryW(L"user32.dll");
            if (user32 != nullptr)
            {
                using SetProcessDpiAwarenessContext_t = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
                auto SetProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContext_t>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
                if (SetProcessDpiAwarenessContext != nullptr)
                {
                    dpi_set = true;
                    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                }

                data->win32_GetDpiForWindow = reinterpret_cast<PFGetDpiForWindow>(GetProcAddress(user32, "GetDpiForWindow"));
                FreeLibrary(user32);
            }

            if (not dpi_set)
            {
                // windows 8.1
                HMODULE shcore = LoadLibraryW(L"shcore.dll");
                if (shcore != nullptr)
                {
                    using SetProcessDpiAwareness_t = HRESULT (WINAPI*)(int);
                    auto SetProcessDpiAwareness = reinterpret_cast<SetProcessDpiAwareness_t>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
                    if (SetProcessDpiAwareness != nullptr)
                    {
                        dpi_set = true;
                        SetProcessDpiAwareness(2);
                    }
                    FreeLibrary(shcore);
                }
            }

            if (not dpi_set)
            {
                // windows 7
                SetProcessDPIAware();
            }
        }
        // Create application window.
        WNDCLASSEXW wc = { sizeof(wc), CS_OWNDC, wnd_proc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"gap", nullptr };
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        ::RegisterClassExW(&wc);

        wind_rect.x = wind_rect.x == default_window_pos.x ? CW_USEDEFAULT : wind_rect.x;
        wind_rect.y = wind_rect.y == default_window_pos.y ? CW_USEDEFAULT : wind_rect.y;
        HWND hwnd = ::CreateWindowExW(WS_EX_APPWINDOW,
                                        wc.lpszClassName,
                                        utf16_title.str,
                                        WS_OVERLAPPEDWINDOW | WS_SIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX,
                                        wind_rect.x, wind_rect.y,
                                        wind_rect.z,
                                        wind_rect.a,
                                        nullptr,
                                        nullptr,
                                        wc.hInstance,
                                        nullptr);
        DragAcceptFiles(hwnd, 1);
        PROF_END(wnd_core_ctx);

        data->wc = wc;
        data->screen_size = ScreenDimensions{ Width{ wind_rect.z }, Height{ wind_rect.a } };
        OSWindow result_wnd = os_window(hwnd);

        // Initialize renderer.
        PROF_BEGIN(wnd_rend_ctx, "WIN32 - Renderer init");
        if (!Render::os_init_renderer_window(result_wnd))
        {
            Render::os_destroy_renderer_window(result_wnd);
            ::DestroyWindow(hwnd);
            ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
            fprintf(stderr, "Unable to initialize platform rendering\n");
            result_wnd = OSWindow::Sentinel;
        }
        PROF_END(wnd_rend_ctx);

        if (result_wnd != OSWindow::Sentinel)
        {
            PROF_BEGIN(os_select_renderer_ctx, "os_select_renderer");
            Render::os_select_renderer(result_wnd);
            PROF_END(os_select_renderer_ctx);

            PROF_BEGIN(show_window_ctx, "ShowWindow/UpdateWindow");
            // Show the window
            ::ShowWindow(hwnd, SW_SHOWDEFAULT);
            ::UpdateWindow(hwnd);
            PROF_END(show_window_ctx);

            GetWindowPlacement(hwnd, &data->win_place);

            // Initialize internal platform data.
            init_win32_wnd(hwnd);
        }

        Arena::scratch_end(scratch);
        return result_wnd;
    }

    void window_minimum_size(ScreenDimensions min_size)
    {
        win32_data()->min_window_size = min_size;
    }

    void destroy_window(OSWindow wind)
    {
        HWND hwnd = win32_window(wind);
        Win32BackendData* data = win32_data();
        // For now, we only handle a single window.
        assert(data->wnd == hwnd);

        Render::os_destroy_renderer_window(wind);
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(data->wc.lpszClassName, data->wc.hInstance);
    }
#endif // WIN32_GFX

    bool window_minimized(OSWindow wind)
    {
        return ::IsIconic(win32_window(wind));
    }

    bool window_maximized(OSWindow wind)
    {
        return ::IsZoomed(win32_window(wind));
    }

    void window_maximize(OSWindow wind)
    {
        ::ShowWindow(win32_window(wind), SW_MAXIMIZE);
    }

    void window_restore(OSWindow wind)
    {
        ::ShowWindow(win32_window(wind), SW_RESTORE);
    }

    // Fullscreening logic largely borrowed from the great Raymond Chen: https://devblogs.microsoft.com/oldnewthing/20100412-00/?p=14353.
    bool window_fullscreened(OSWindow wind)
    {
        HWND hwnd = win32_window(wind);
        DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
        return not (style & WS_OVERLAPPEDWINDOW);
    }

    void window_fullscreen(OSWindow wind)
    {
        Win32BackendData* data = win32_data();
        HWND hwnd = win32_window(wind);
        MONITORINFO mi = { sizeof(mi) };
        DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
        if (GetWindowPlacement(hwnd, &data->win_place)
            and GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP,
                            mi.rcMonitor.left, mi.rcMonitor.top,
                            mi.rcMonitor.right - mi.rcMonitor.left,
                            mi.rcMonitor.bottom - mi.rcMonitor.top,
                            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }

    void window_windowed(OSWindow wind)
    {
        Win32BackendData* data = win32_data();
        HWND hwnd = win32_window(wind);
        DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &data->win_place);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }

    Vec4i window_rect(OSWindow wind)
    {
        RECT rect = {};
        GetWindowRect(win32_window(wind), &rect);
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        return { rect.left, rect.top, w, h };
    }

#ifdef WIN32_GFX
    void swap_buffers([[maybe_unused]] OSWindow wind)
    {
        Win32BackendData* data = win32_data();
        GAP_UNUSED(data);
        // For now, assume we're swapping with the core window.
        assert(win32_window(wind) == data->wnd);
        Render::os_swap_buffers(wind);
    }
#endif // WIN32_GFX

    OSWindow core_window()
    {
        return os_window(win32_data()->wnd);
    }

    // Largely borrowed from: https://stackoverflow.com/questions/39261826/change-the-color-of-the-title-bar-caption-of-a-win32-application.
    Error apply_window_border_color(OSWindow wind, const Vec4f& color)
    {
        HWND wnd = win32_window(wind);
        COLORREF cref = as_colorref(color);
        HRESULT result = DwmSetWindowAttribute(wnd,
                                DWMWINDOWATTRIBUTE::DWMWA_CAPTION_COLOR,
                                &cref,
                                sizeof(cref));
        if (not SUCCEEDED(result))
            return Error::FailedToApplyWindowCaptionColor;
        return Error::None;
    }

    Error apply_title_font_color(OSWindow wind, const Vec4f& color)
    {
        HWND wnd = win32_window(wind);
        COLORREF cref = as_colorref(color);
        HRESULT result = DwmSetWindowAttribute(wnd,
                                DWMWINDOWATTRIBUTE::DWMWA_TEXT_COLOR,
                                &cref,
                                sizeof(cref));
        if (not SUCCEEDED(result))
            return Error::FailedToApplyWindowFontColor;
        return Error::None;
    }

    // Event processing.
    void query_events(Arena::Arena* arena, Events* lst, Wait wait)
    {
        Win32BackendData* data = win32_data();
        data->caller_event_queue = lst;
        data->win32_event_arena = arena;
        MSG msg{};
        // If we already have events queued up, let's try to get some more, but not wait on the API to return.
        if (data->caller_event_queue->count != 0)
        {
            wait = Wait::No;
        }

        if (is_no(wait) or GetMessageW(&msg, 0, 0, 0))
        {
            bool first_wait = is_yes(wait);
            for (;first_wait or PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); first_wait = false)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                // These messages are not associated with any window so they will not be dispatched via the usual wnd_proc method.
                switch (msg.message)
                {
                case WM_QUIT:
                    {
                        auto window_handle = os_window(data->wnd);
                        push_event(EventSort::Quit, window_handle);
                    }
                    break;

                // Custom events.
                case rep(CustomEvents::ThreadWakeup):
                    {
                        auto window_handle = os_window(data->wnd);
                        push_event(EventSort::GapThreadWakeup, window_handle);
                    }
                    break;
                }
            }
        }
    }

    // Direct Interaction.
    // Mouse cursor.
    void set_cursor(CursorStyle style)
    {
        Win32BackendData* data = win32_data();
        ::SetCursor(data->cursors[rep(style)]);
        data->current_cursor = style;
    }

    bool delta_meets_double_click_time(Ticks start, Ticks end)
    {
        if (start > end)
            return false;
        auto double_click_time = GetDoubleClickTime();
        return ((rep(end) - rep(start)) <= double_click_time);
    }

    bool delta_meets_double_click_time(Ticks32 start, Ticks32 end)
    {
        if (start > end)
            return false;
        auto double_click_time = GetDoubleClickTime();
        return ((rep(end) - rep(start)) <= double_click_time);
    }

    // Clipboard.
    Error clipboard_text(Arena::Arena* arena, String8* result)
    {
        *result = String8{};
        if (not IsClipboardFormatAvailable(CF_UNICODETEXT))
            return Error::ClipboardTextUnavailable;

        if (not OpenClipboard(nullptr))
            return Error::UnableToOpenClipboard;

        HANDLE mem = GetClipboardData(CF_UNICODETEXT);
        Error error = Error::None;
        if (not mem)
        {
            error = Error::InvalidClipboardData;
        }
        else
        {
            LPCWCH str = static_cast<LPCWCH>(GlobalLock(mem));
            *result = convert_utf16_to_utf8(arena, str16_cstr(const_cast<wchar_t*>(str)));
            GlobalUnlock(mem);
        }
        CloseClipboard();
        return error;
    }

    Error set_clipboard(String8 buf)
    {
        if (not OpenClipboard(nullptr))
            return Error::UnableToOpenClipboard;
        if (not EmptyClipboard())
        {
            CloseClipboard();
            return Error::EmptyClipboardFailed;
        }
        // Create a temporary buffer for the wide-string.
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 utf16 = convert_utf8_to_utf16(scratch.arena, buf);
        auto glb_mem = copy_to_global_reconcile_crlf(utf16);
        // This now owns the memory and we do not need to free it
        // (https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboarddata?redirectedfrom=MSDN#parameters).
        SetClipboardData(CF_UNICODETEXT, glb_mem);
        CloseClipboard();
        Arena::scratch_end(scratch);
        return Error::None;
    }

    Error set_clipboard_html(String8 buf, String8 html)
    {
        if (not OpenClipboard(nullptr))
            return Error::UnableToOpenClipboard;
        auto clip_fmt = RegisterClipboardFormatA("HTML Format");
        if (not EmptyClipboard())
        {
            CloseClipboard();
            return Error::EmptyClipboardFailed;
        }
        // Create a temporary buffer for the wide-string.
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 utf16 = convert_utf8_to_utf16(scratch.arena, buf);
        auto glb_mem = copy_to_global_reconcile_crlf(utf16);
        // This now owns the memory and we do not need to free it
        // (https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboarddata?redirectedfrom=MSDN#parameters).
        SetClipboardData(CF_UNICODETEXT, glb_mem);
        // Now copy the HTML values.  Note these are always copied as UTF-8
        // (https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa767917(v=vs.85)#description)
        String8 headered_html = generate_html_with_header(scratch.arena, html);
        glb_mem = copy_to_global_reconcile_crlf(headered_html);
        SetClipboardData(clip_fmt, glb_mem);
        CloseClipboard();
        Arena::scratch_end(scratch);
        return Error::None;
    }

    ClipboardIdentity clipboard_id()
    {
        return ClipboardIdentity{ GetClipboardSequenceNumber() };
    }

    // Queries.
    // System information.
    const SystemInfo* system_info()
    {
        Win32BackendData* data = win32_data();
        return &data->sys_info;
    }

    // Time.
    Ticks get_ticks()
    {
        LARGE_INTEGER now;
        BOOL rc;

        rc = QueryPerformanceCounter(&now);
        auto* bd = win32_data();
        return static_cast<Ticks>(((now.QuadPart - bd->start_time.QuadPart) * Thousand(1)) / bd->frequency.QuadPart);
    }

    Ticks32 get_ticks32()
    {
        return static_cast<Ticks32>(get_ticks());
    }

    MicroSec now_microseconds()
    {
        LARGE_INTEGER now;
        MicroSec result = {};
        if (QueryPerformanceCounter(&now))
        {
            auto* bd = win32_data();
            result = static_cast<MicroSec>(((now.QuadPart) * Million(1)) / bd->frequency.QuadPart);
        }
        return result;
    }

    // Monitor.
    Hz monitor_refresh_rate()
    {
        Win32BackendData* data = win32_data();
        return data->refresh_rate;
    }

    Hz recompute_monitor_refresh_rate()
    {
        DEVMODEW modew{};
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &modew))
        {
            return Hz{ modew.dmDisplayFrequency };
        }
        return Hz::Default;
    }

    ScreenDimensions window_size()
    {
        return win32_data()->screen_size;
    }

    ScreenDimensions client_size()
    {
        RECT rect{};
        Win32BackendData* data = win32_data();
        GetClientRect(data->wnd, &rect);
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        return ScreenDimensions{ Width{ w }, Height{ h } };
    }

    DPI monitor_dpi()
    {
        Win32BackendData* data = win32_data();
        if (data->win32_GetDpiForWindow != nullptr)
            return DPI{ data->win32_GetDpiForWindow(data->wnd) };
        return DPI{ 96 };
    }

    // Paths.
    Error exe_path(Arena::Arena* arena, String8* buf)
    {
        wchar_t wbuf[internal_max_path];
        constexpr auto buf_size = std::size(wbuf);
        DWORD len = GetModuleFileNameW(nullptr, wbuf, buf_size);
        auto error = Error::None;
        if (len > buf_size)
        {
            // Despite the error, we can still continue.
            error = Error::BufferTruncated;
        }
        // Note: len includes the null terminator, so we'll back it up.
        // https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamea#return-value
        --len;
        // Chop at the last slash.
        for (int i = len; i > 0; --i)
        {
            if (wbuf[i] == L'\\')
                break;
            --len;
        }
        assert(len > 0);
        // Leave trailing slash (located at 'len - 1').
        wbuf[len] = '\0';
        *buf = convert_utf16_to_utf8(arena, str16(wbuf, len));
        return error;
    }

    FileHandle open_file(String8 file, FileAccess access)
    {
        // Note: For directory reading to work from here, the file access should only be: GENERIC_READ, FILE_SHARE_READ, and the
        // attributes set to FILE_FLAG_BACKUP_SEMANTICS.
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wfile = convert_utf8_to_utf16(scratch.arena, file);
        DWORD access_flags = 0;
        DWORD share_mode = 0;
        DWORD disposition = OPEN_EXISTING;
        DWORD attrs = FILE_ATTRIBUTE_NORMAL;
        SECURITY_ATTRIBUTES security_attributes = { sizeof(security_attributes), 0, 0 };
        if(implies(access, FileAccess::Read))       { access_flags |= GENERIC_READ; }
        if(implies(access, FileAccess::Write))      { access_flags |= GENERIC_WRITE; }
        if(implies(access, FileAccess::Execute))    { access_flags |= GENERIC_EXECUTE; }
        if(implies(access, FileAccess::ShareRead))  { share_mode |= FILE_SHARE_READ; }
        if(implies(access, FileAccess::ShareWrite)) { share_mode |= FILE_SHARE_WRITE|FILE_SHARE_DELETE; }
        if(implies(access, FileAccess::Write))      { disposition = CREATE_ALWAYS; }
        if(implies(access, FileAccess::Append))     { disposition = OPEN_ALWAYS; access_flags |= FILE_APPEND_DATA; }
        if(implies(access, FileAccess::IsDir))      { attrs = FILE_FLAG_BACKUP_SEMANTICS; }
        if(implies(access, FileAccess::Inherited))
        {
            security_attributes.bInheritHandle = 1;
        }
        HANDLE file_handle = CreateFileW(wfile.str,
                                    access_flags,
                                    share_mode,
                                    &security_attributes,
                                    disposition,
                                    attrs,
                                    0);
        Arena::scratch_end(scratch);
        if (file_handle == INVALID_HANDLE_VALUE)
            return FileHandle::Sentinel;
        return os_file_handle(file_handle);
    }

    void close_file(FileHandle handle)
    {
        assert(handle != FileHandle::Sentinel);
        CloseHandle(win32_file_handle(handle));
    }

    FileLength file_length(FileHandle handle)
    {
        assert(handle != FileHandle::Sentinel);
        HANDLE h = win32_file_handle(handle);
        LARGE_INTEGER size{};
        if (not GetFileSizeEx(h, &size))
            return FileLength::Sentinel;
        // We have to truncate this.  If there are files which are larger than
        // uint64_t... idk.  Maybe we just crash.  Idk.
        return FileLength(size.QuadPart);
    }

    FileLength read_file(Arena::Arena* arena, String8* buf, FileHandle handle, FileLength count)
    {
        assert(handle != FileHandle::Sentinel);
        HANDLE h = win32_file_handle(handle);
        // Loop and read in chunks.
        *buf = str8_cstr_alloc(arena, rep(count));
        uint64_t total_read_size = 0;
        for (uint64_t off = 0; total_read_size < rep(count);)
        {
            uint64_t read_64 = rep(count) - total_read_size;
            uint32_t read_32 = u32_from_u64_saturate(read_64);
            DWORD read_size = 0;
            OVERLAPPED overlapped{};
            overlapped.Offset     = (off & 0x00000000ffffffffull);
            overlapped.OffsetHigh = (off & 0xffffffff00000000ull) >> 32;
            ReadFile(h, buf->str + total_read_size, read_32, &read_size, &overlapped);
            off += read_size;
            total_read_size += read_size;
            if (read_size != read_32)
                break;
        }
        return FileLength{ total_read_size };
    }

    BytesWritten write_file(FileHandle handle, FileOffset off, String8 buf)
    {
        assert(handle != FileHandle::Sentinel);
        HANDLE h = win32_file_handle(handle);
        uint64_t buf_off = 0;
        uint64_t dest_off = rep(off);
        uint64_t total_write_size = buf.size;
        for (;;)
        {
            const void* bytes_src = buf.str + buf_off;
            uint64_t bytes_left = total_write_size - buf_off;
            DWORD write_size = static_cast<DWORD>((std::min)(MB(1), bytes_left));
            DWORD bytes_written = 0;
            OVERLAPPED overlapped{};
            overlapped.Offset     = (dest_off & 0x00000000ffffffffull);
            overlapped.OffsetHigh = (dest_off & 0xffffffff00000000ull) >> 32;
            BOOL success = WriteFile(h, bytes_src, write_size, &bytes_written, &overlapped);
            if (not success)
                break;
            buf_off += bytes_written;
            dest_off += bytes_written;
            if (bytes_left == 0)
                break;
        }
        return BytesWritten{ buf_off };
    }

    FileProperties file_properties(String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        WIN32_FIND_DATAW find_data = {0};
        String16 wpath = convert_utf8_to_utf16(scratch.arena, path);
        HANDLE handle = FindFirstFileW(wpath.str, &find_data);
        FileProperties props{ };
        if(handle != INVALID_HANDLE_VALUE)
        {
            populate_file_props_from_find_data(&props, find_data);
        }
        FindClose(handle);
        Arena::scratch_end(scratch);
        return props;
    }

    Error create_directory(String8 dir)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wdir = convert_utf8_to_utf16(scratch.arena, dir);
        bool create_result = not not CreateDirectoryW(wdir.str, nullptr);
        Arena::scratch_end(scratch);
        if (not create_result)
        {
            auto last_err = GetLastError();
            if (last_err != ERROR_ALREADY_EXISTS)
                return Error::CreateDirectoryFailed;
        }
        return Error::None;
    }

    Error app_path(Arena::Arena* arena, String8* buf, String8 org, String8 app)
    {
        WCHAR path[internal_max_path];
        assert(org.size != 0);
        assert(app.size != 0);
        auto scratch = Arena::scratch_begin({ &arena, 1 });

        auto err = Error::None;
        if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, path)))
        {
            err = Error::CouldNotLocatePrefPath;
        }

        String8List core_path{};
        String16 converted = {};
        if (err == Error::None)
        {
            String8 base = convert_utf16_to_utf8(scratch.arena, str16_cstr(path));
            str8_list_push(scratch.arena, &core_path, base);
            str8_list_push(scratch.arena, &core_path, str8_mut(str8_literal("\\")));
            str8_list_push(scratch.arena, &core_path, org);
            String8 joined = str8_list_join(scratch.arena, core_path);
            converted = convert_utf8_to_utf16(scratch.arena, joined);
            if (CreateDirectoryW(converted.str, nullptr) == FALSE)
            {
                auto last_err = GetLastError();
                if (last_err != ERROR_ALREADY_EXISTS)
                {
                    err = Error::CreateDirectoryFailed;
                }
            }
        }

        if (err == Error::None)
        {
            str8_list_push(scratch.arena, &core_path, str8_mut(str8_literal("\\")));
            str8_list_push(scratch.arena, &core_path, app);
            String8 joined = str8_list_join(scratch.arena, core_path);
            converted = convert_utf8_to_utf16(scratch.arena, joined);
            if (CreateDirectoryW(converted.str, nullptr) == FALSE)
            {
                auto last_err = GetLastError();
                if (last_err != ERROR_ALREADY_EXISTS)
                {
                    err = Error::CreateDirectoryFailed;
                }
            }
            str8_list_push(scratch.arena, &core_path, str8_mut(str8_literal("\\")));
            *buf = str8_list_join(arena, core_path);
        }
        Arena::scratch_end(scratch);
        return err;
    }

    bool directory_exists(String8 dir)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wdir = convert_utf8_to_utf16(scratch.arena, dir);
        DWORD attrs = GetFileAttributesW(wdir.str);
        Arena::scratch_end(scratch);
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return false;
        return attrs & FILE_ATTRIBUTE_DIRECTORY;
    }

    bool file_or_path_exists(String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wpath = convert_utf8_to_utf16(scratch.arena, path);
        DWORD attrs = GetFileAttributesW(wpath.str);
        Arena::scratch_end(scratch);
        return attrs != INVALID_FILE_ATTRIBUTES;
    }

    bool regular_file_exists(String8 file)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wfile = convert_utf8_to_utf16(scratch.arena, file);
        DWORD attrs = GetFileAttributesW(wfile.str);
        Arena::scratch_end(scratch);
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return false;
        // It cannot be a reparse point (symlink) or directory.
        return not(attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    }

    Error canonical_file_path(Arena::Arena* arena, String8* buf, String8 path)
    {
        FileAccess access = FileAccess::Read | FileAccess::ShareRead;
        if (directory_exists(path))
            access |= FileAccess::IsDir;
        auto handle = open_file(path, access);
        if (handle == FileHandle::Sentinel)
            return Error::CanonicalizeFailedToOpenFile;
        UTF16FileBuffer file_buf;
        DWORD result = GetFinalPathNameByHandleW(win32_file_handle(handle), file_buf.buf, std::size(file_buf.buf), FILE_NAME_NORMALIZED);
        // We can close the file handle now.
        close_file(handle);
        if (not result)
            return Error::CanonicalizeFailedToGetPathname;
        remove_filesystem_artifacts(&file_buf);
        *buf = convert_utf16_to_utf8(arena, str16_cstr(file_buf.buf));
        return Error::None;
    }

    bool working_directory(Arena::Arena* arena, String8* buf)
    {
        wchar_t wbuf[internal_max_path];
        DWORD bytes_w = GetCurrentDirectoryW(internal_max_path, wbuf);
        if (bytes_w == 0)
            return false;
        *buf = convert_utf16_to_utf8(arena, str16(wbuf, bytes_w));
        return true;
    }

    bool set_working_directory(String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wpath = convert_utf8_to_utf16(scratch.arena, path);
        bool result = not not SetCurrentDirectoryW(wpath.str);
        Arena::scratch_end(scratch);
        return result;
    }

    // Directory iteration.
    DirIter open_dir_iter(String8 dir, DirIterFlags flags)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Append the wildcard.
        String8 dir_w_wildcard = str8_cat(scratch.arena, dir, str8_mut(str8_literal("\\*")));
        String16 wdir = convert_utf8_to_utf16(scratch.arena, dir_w_wildcard);
        Win32BackendData* data = win32_data();
        // Allocate a new dir iter.
        Win32Entity* e = alloc_dir_iter_data(data);
        DirIterData* dir_data = &e->dir_iter_data;
        DirIter result = os_dir_iter(e);
        // Open the directory.
        dir_data->handle = FindFirstFileW(wdir.str, &dir_data->find_data);
        if (dir_data->handle != INVALID_HANDLE_VALUE)
        {
            dir_data->flags = flags;
            CoreDirData* core_dir = &dir_data->core_dir->core_dir;
            core_dir->size = std::min(std::size(core_dir->core_dir), dir.size);
            memcpy(core_dir->core_dir, dir.str, core_dir->size);
        }
        else
        {
            release_dir_iter_data(data, e);
            result = DirIter::Sentinel;
        }
        Arena::scratch_end(scratch);
        return result;
    }

    bool dir_iter_next(Arena::Arena* arena, DirIterResult* result, DirIter iter)
    {
        assert(iter != DirIter::Sentinel);
        Win32Entity* e = win32_dir_iter(iter);
        DirIterData* dir_data = &e->dir_iter_data;
        if (dir_data->handle == INVALID_HANDLE_VALUE)
            return false;
        if (implies(dir_data->flags, DirIterFlags::Done))
            return false;
        bool found = false;
        do
        {
            bool usable = true;
            String16 name = str16_cstr(dir_data->find_data.cFileName);
            DWORD attrs = dir_data->find_data.dwFileAttributes;
            // Exclude meta directories.
            if (str16_match_exact(name, str16_mut(str16_literal(L".")))
                or str16_match_exact(name, str16_mut(str16_literal(L".."))))
            {
                usable = false;
            }

            if (attrs & FILE_ATTRIBUTE_DIRECTORY)
            {
                usable = usable and not implies(dir_data->flags, DirIterFlags::SkipDirs);
            }
            else
            {
                usable = usable and not implies(dir_data->flags, DirIterFlags::SkipFiles);
            }

            // Populate result and finish.
            if (usable)
            {
                found = true;
                if (implies(dir_data->flags, DirIterFlags::FullPath))
                {
                    auto scratch = Arena::scratch_begin({ &arena, 1 });
                    String8 u8name = convert_utf16_to_utf8(scratch.arena, name);
                    String8 core_dir = str8(dir_data->core_dir->core_dir.core_dir,
                                            dir_data->core_dir->core_dir.size);
                    result->path = combine_paths(arena, core_dir, u8name);
                    Arena::scratch_end(scratch);
                }
                else
                {
                    result->path = convert_utf16_to_utf8(arena, name);
                }
                populate_file_props_from_find_data(&result->props, dir_data->find_data);
                // Move the iterator forward.
                if (not FindNextFileW(dir_data->handle, &dir_data->find_data))
                {
                    dir_data->flags |= DirIterFlags::Done;
                }
                break;
            }
        } while(FindNextFileW(dir_data->handle, &dir_data->find_data));
        if (not found)
        {
            dir_data->flags |= DirIterFlags::Done;
        }
        return found;
    }

    void close_dir_iter(DirIter iter)
    {
        assert(iter != DirIter::Sentinel);
        Win32BackendData* data = win32_data();
        Win32Entity* dir_data = win32_dir_iter(iter);
        release_dir_iter_data(data, dir_data);
    }

    String8 parent_path(String8 path)
    {
        // Find the root path.
        auto root_pos = path_root_end(path);
        String8 relative_path = str8_substr(path, { .off = root_pos });
        // Case 1: Relative path ends in a directory separator, e.g. /root/dir/
        //         so we remove the separator and return /root/.
        // Case 2: We have a relative path which doesn't end in a directory separator
        //         e.g. /root/dir so we'll remove 'dir' and the trailing slash.
        const char* first = relative_path.str;
        const char* last = first + relative_path.size;
        // Handle case 2 by removing the trailing filename.
        while (first != last and not path_separator(*(last - 1)))
        {
            --last;
        }
        // Case 1.
        while (first != last and path_separator(*(last - 1)))
        {
            --last;
        }
        return str8_substr(path, { .off = 0, .len = static_cast<uint64_t>(last - path.str) });
    }

    String8 combine_paths(Arena::Arena* arena, String8 a, String8 b)
    {
        // First separate the two paths by the either '/' or '\' and then combine
        // them together.
        auto scratch = Arena::scratch_begin({ &arena, 1 });
        String8List split{};
        String8 path_seps = str8_mut(str8_literal("/\\"));
        SplitStringsInput split_in{
            .in = a,
            .seps = path_seps,
            .flags = SplitStringsFlags::NoResultClear // We want to append results.
        };
        split_strings(scratch.arena, &split, split_in);
        split_in.in = b;
        split_strings(scratch.arena, &split, split_in);
        // Combine them all together using our preferred system separator.
        JoinStringsInput join_in{
            .strings = split,
            .start_sep = str8_mut(str8_literal("")),
            .sep = str8_mut(str8_literal(PATH_SEP)),
            .end_sep = str8_mut(str8_literal("")) // Don't append a separator...
        };
        String8 result = join_strings(arena, join_in);
        Arena::scratch_end(scratch);
        return result;
    }

    String8 root_of_path(String8 path)
    {
        auto end_pos = path_root_end(path);
        // Include the extra slash as well, if we're provided one.
        if (end_pos < path.size and path_separator(path.str[end_pos]))
        {
            ++end_pos;
        }
        return str8_substr(path, { .len = end_pos });
    }

    // External command invocation.
    void open_url_in_browser(String8 url)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wurl = convert_utf8_to_utf16(scratch.arena, url);
        ShellExecuteW(nullptr, L"open", wurl.str, 0, 0, SW_SHOWNORMAL);
        Arena::scratch_end(scratch);
    }

    void open_path_in_explorer(String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wpath = convert_utf8_to_utf16(scratch.arena, path);
        ShellExecuteW(nullptr, L"explore", wpath.str, 0, 0, SW_SHOWNORMAL);
        Arena::scratch_end(scratch);
    }

    void open_path_in_preferred_explorer(String8 explorer, String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String8List cmd_line{};
        str8_list_push(scratch.arena, &cmd_line, explorer);
        str8_list_push(scratch.arena, &cmd_line, path);
        String8 exe_path = str8_empty;
        OS::exe_path(scratch.arena, &exe_path);
        ProcessLaunchParams in{
            .wd = exe_path,
            .cmd_line = cmd_line,
            .env = {},
            .flags = LaunchFlags::InheritEnv | LaunchFlags::Consoleless
        };
        auto process = launch_process(in);
        Arena::scratch_end(scratch);
        if (process != ProcessHandle::Sentinel)
        {
            detach_process(process);
        }
    }

    // Gap-specific events.
    void post_thread_wakeup()
    {
        Win32BackendData* data = win32_data();
        constexpr CustomEvents e = convert(EventSort::GapThreadWakeup);
        PostThreadMessageA(data->ui_thread_id, rep(e), 0, 0);
    }

    // Library handling.
    LibraryHandle load_library(String8 lib_name)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String16 wlib = convert_utf8_to_utf16(scratch.arena, lib_name);
        HMODULE lib = LoadLibraryW(wlib.str);
        Arena::scratch_end(scratch);
        if (lib == nullptr)
            return LibraryHandle::Sentinel;
        return os_library_handle(lib);
    }

    void unload_library(LibraryHandle lib)
    {
        FreeLibrary(win32_library_handle(lib));
    }

    void* get_function(LibraryHandle lib, String8 fn)
    {
        // Note: Not sure if we need to worry about the view not being null-terminated.
        return GetProcAddress(win32_library_handle(lib), fn.str);
    }

#ifdef WIN32_GFX
#ifdef BUILD_OPENGL_RENDERER
    LibraryHandle get_gl_library()
    {
        return OS::load_library(str8_mut(str8_literal("opengl32.dll")));
    }

    void* get_gl_function(LibraryHandle gl_lib, String8 fn)
    {
        // OpenGL functions can exist either in the GL library or as an extension through
        // wglGetProcAddress.
        void* pfn = get_function(gl_lib, fn);
        if (pfn != nullptr)
            return pfn;
        return wglGetProcAddress(fn.str);
    }
#endif // BUILD_OPENGL_RENDERER
#endif // WIN32_GFX

    // Process creation.
    ProcessHandle launch_process(const ProcessLaunchParams& in)
    {
        CreateProcessParams params{};
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        create_process_params(scratch.arena, &params, in);

        // Setup start info.
        BOOL inherit_handles = FALSE;
        STARTUPINFOW start_info = { sizeof(start_info) };
        if (in.stdout_file != FileHandle::Sentinel)
        {
            HANDLE stdout_file = win32_file_handle(in.stdout_file);
            start_info.hStdOutput = stdout_file;
            start_info.dwFlags |= STARTF_USESTDHANDLES;
            inherit_handles = TRUE;
        }

        if (in.stdin_file != FileHandle::Sentinel)
        {
            HANDLE stdin_file = win32_file_handle(in.stdin_file);
            start_info.hStdOutput = stdin_file;
            start_info.dwFlags |= STARTF_USESTDHANDLES;
            inherit_handles = TRUE;
        }

        if (in.stderr_file != FileHandle::Sentinel)
        {
            HANDLE stderr_file = win32_file_handle(in.stderr_file);
            start_info.hStdOutput = stderr_file;
            start_info.dwFlags |= STARTF_USESTDHANDLES;
            inherit_handles = TRUE;
        }

        ProcessHandle handle = ProcessHandle::Sentinel;
        PROCESS_INFORMATION process_info{ };

        LPCWSTR cmd = nullptr;
        if (implies(in.flags, LaunchFlags::CLIProcess))
        {
            cmd = L"C:\\Windows\\System32\\cmd.exe";
        }

        if (CreateProcessW(cmd,
                            params.wargs.str,
                            nullptr,
                            nullptr,
                            inherit_handles,
                            params.create_flags,
                            params.env_ptr,
                            params.wwd.str,
                            &start_info,
                            &process_info))
        {
            handle = os_process_handle(process_info.hProcess);
            // Discard the thread.
            CloseHandle(process_info.hThread);
        }
        Arena::scratch_end(scratch);
        return handle;
    }

    IOPipe launch_piped_process(const ProcessLaunchParams& in)
    {
        IOPipe ret = IOPipe::Sentinel;

        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        CreateProcessParams params{};
        create_process_params(scratch.arena, &params, in);

        // Regardless of what 'in' says, we will suspend this process.
        params.create_flags |= CREATE_SUSPENDED;

        // Create the IO handles.
        // Input pipes.
        PipePair in_pipes = create_named_pipes();
        if (not valid_pipe(in_pipes))
        {
            Arena::scratch_end(scratch);
            return ret;
        }

        // Output pipes.
        PipePair out_pipes = create_named_pipes();
        if (not valid_pipe(out_pipes))
        {
            Arena::scratch_end(scratch);
            close_pipes(in_pipes);
            return ret;
        }

        // Error pipes.
        PipePair err_pipes = create_named_pipes();
        if (not valid_pipe(err_pipes))
        {
            Arena::scratch_end(scratch);
            close_pipes(in_pipes);
            close_pipes(out_pipes);
            return ret;
        }

        // Setup start info.
        BOOL inherit_handles = TRUE;
        STARTUPINFOW start_info = { sizeof(start_info) };
        start_info.dwFlags = STARTF_USESTDHANDLES;
        start_info.hStdInput = in_pipes.read;
        start_info.hStdOutput = out_pipes.write;
        start_info.hStdError = err_pipes.write;

        // Ignore input files.

        // Let's also assign a job object to this so we can nuke anything under the cmd.exe.
        HANDLE job_h = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                                                    | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        SetInformationJobObject(job_h, JobObjectExtendedLimitInformation, &job_info, sizeof(job_info));

        PROCESS_INFORMATION process_info{ };

        LPCWSTR cmd = nullptr;
        if (implies(in.flags, LaunchFlags::CLIProcess))
        {
            cmd = L"C:\\Windows\\System32\\cmd.exe";
        }

        if (CreateProcessW(cmd,
                            params.wargs.str,
                            nullptr,
                            nullptr,
                            inherit_handles,
                            params.create_flags,
                            params.env_ptr,
                            params.wwd.str,
                            &start_info,
                            &process_info))
        {
            // Set this process as the parent this way if we kill cmd.exe, it will take down any
            // children spawned with it.
            AssignProcessToJobObject(job_h, process_info.hProcess);

            // Now that we've created all the pipes, we can resume the thread.
            ResumeThread(process_info.hThread);
            // Discard the thread.
            CloseHandle(process_info.hThread);
            // Discard irrelevant pipes.
            close_pipes(in_pipes);
            CloseHandle(out_pipes.write);
            CloseHandle(err_pipes.write);
            // We only need the read side of out and err pipes.
            // Create events for the pipe data.
            Win32BackendData* data = win32_data();
            AllocPipeDataInput alloc_in = {
                .process = process_info.hProcess,
                .job = job_h,
                .stdout_pipe = out_pipes.read,
                .stderr_pipe = err_pipes.read
            };
            Win32Entity* pipe_data = alloc_pipe_data(data, alloc_in);
            Arena::scratch_end(scratch);
            return os_pipe_handle(pipe_data);
        }
        Arena::scratch_end(scratch);
        close_pipes(in_pipes);
        close_pipes(out_pipes);
        close_pipes(err_pipes);
        CloseHandle(job_h);
        return ret;
    }

    ReadIOPipeResult read_piped_process(IOPipe piped_process, ReadIOPipeInput in)
    {
        // The logic here was largely pulled from: https://gist.github.com/mmozeiko/b4e55a25aeb291ddd9bca1d8d228ea95
        ReadIOPipeResult result{};
        Win32Entity* e = win32_pipe_handle(piped_process);
        IOPipeData* pipe_data = &e->pipe_data;
        if (pipe_data->count == 0)
        {
            result.exit_code = pipe_data->exit_code;
            result.pending_output = false;
            Win32BackendData* data = win32_data();
            // We can finally close the job.
            CloseHandle(pipe_data->job);
            release_pipe_data(data, e);
            return result;
        }

        // Assuming always pending until all handles are complete (count == 0).
        result.pending_output = true;
        // Return immediately.
        DWORD wait = WaitForMultipleObjects(pipe_data->count, pipe_data->events, FALSE, 0);
        if (wait == WAIT_TIMEOUT)
            return result;
        DWORD idx = wait - WAIT_OBJECT_0;
        HANDLE h = pipe_data->events[idx];

        if (h == pipe_data->stdout_ep->pipe_ep.event)
        {
            IOPipeEndpoint* ep = &pipe_data->stdout_ep->pipe_ep;
            result.std_out_write = read_pipe_endpoint(in.arena, in.std_out, pipe_data, ep, idx);
        }
        else if (h == pipe_data->stderr_ep->pipe_ep.event)
        {
            IOPipeEndpoint* ep = &pipe_data->stderr_ep->pipe_ep;
            result.std_err_write = read_pipe_endpoint(in.arena, in.std_err, pipe_data, ep, idx);
        }
        else if (h == pipe_data->process)
        {
            pipe_data->events[idx] = pipe_data->events[pipe_data->count - 1];
            --pipe_data->count;

            DWORD exit_code;
            GetExitCodeProcess(pipe_data->process, &exit_code);
            CloseHandle(pipe_data->process);
            pipe_data->process = INVALID_HANDLE_VALUE;
            pipe_data->exit_code = exit_code;
        }
        return result;
    }

    void join_process(ProcessHandle handle)
    {
        HANDLE process = win32_process_handle(handle);
        // TODO: Make the wait configurable?
        [[maybe_unused]] DWORD result = WaitForSingleObject(process, INFINITE);
        assert(result == WAIT_OBJECT_0);
    }

    void detach_process(ProcessHandle handle)
    {
        HANDLE process = win32_process_handle(handle);
        CloseHandle(process);
    }

    void terminate_process(ProcessHandle handle)
    {
        HANDLE process = win32_process_handle(handle);
        TerminateProcess(process, 999);
        detach_process(handle);
    }

    void close_pipes_and_terminate_process(IOPipe piped_process)
    {
        Win32Entity* e = win32_pipe_handle(piped_process);
        IOPipeData* pipe_data = &e->pipe_data;
        // Close the pipes.
        CloseHandle(pipe_data->stdout_ep->pipe_ep.read_pipe);
        CloseHandle(pipe_data->stderr_ep->pipe_ep.read_pipe);
        // Let's terminate the process first.
        TerminateJobObject(pipe_data->job, 999);
        pipe_data->process = INVALID_HANDLE_VALUE;
        pipe_data->exit_code = 999;
        // Close our handles.
        CloseHandle(pipe_data->process);
        CloseHandle(pipe_data->job);
        // Finally, we can invalidate the object.
        release_pipe_data(win32_data(), e);
    }

    // Memory allocation.
    void* mem_reserve(AllocationSize size)
    {
        void* result = VirtualAlloc(nullptr, rep(size), MEM_RESERVE, PAGE_READWRITE);
        return result;
    }

    bool mem_commit(void* ptr, AllocationSize size)
    {
        bool result = VirtualAlloc(ptr, rep(size), MEM_COMMIT, PAGE_READWRITE) != 0;
        return result;
    }

    void mem_decommit(void* ptr, AllocationSize size)
    {
        VirtualFree(ptr, rep(size), MEM_DECOMMIT);
    }

    void mem_release(void* ptr, AllocationSize)
    {
        // Note: size is unnecessary.  Windows knows the deallocation size based on pointer address.
        VirtualFree(ptr, 0, MEM_RELEASE);
    }

    void* mem_reserve_large(AllocationSize size)
    {
        // We commit on reserve because... Windows.
        void* result = VirtualAlloc(0, rep(size), MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
        return result;
    }

    bool mem_commit_large(void*, AllocationSize)
    {
        return true;
    }

    void mem_clear_working_set_pages()
    {
        // If both dwMinimumWorkingSetSize and dwMaximumWorkingSetSize have the
        // value (SIZE_T)–1, the function removes as many pages as possible from
        // the working set of the specified process.
        SetProcessWorkingSetSize(GetCurrentProcess(), max_U64, max_U64);
    }

    // Threading.
    Thread launch_thread(ThreadEntryPointFunctionType entry, void* data_p)
    {
        Win32BackendData* data = win32_data();
        Win32Entity* e = win32_alloc_thread(data, entry, data_p);
        return os_thread(e);
    }

    bool join_thread(Thread thread)
    {
        Win32BackendData* data = win32_data();
        Win32Entity* e = win32_thread(thread);
        WaitForSingleObject(e->thread.handle, INFINITE);
        CloseHandle(e->thread.handle);
        win32_release_thread(data, thread);
        return true;
    }

    ThreadID thread_id()
    {
        return ThreadID{ GetCurrentThreadId() };
    }

    // Synchronization primitives.
    // Mutex.
    Mutex alloc_mutex()
    {
        Win32BackendData* data = win32_data();
        Win32Entity* e = win32_mutex_alloc(data);
        return os_mutex(e);
    }

    void release_mutex(Mutex mutex)
    {
        Win32BackendData* data = win32_data();
        release_win32_mutex(data, mutex);
    }

    void lock_mutex(Mutex mutex)
    {
        Win32Entity* e = win32_mutex(mutex);
        EnterCriticalSection(&e->mutex);
    }

    void unlock_mutex(Mutex mutex)
    {
        Win32Entity* e = win32_mutex(mutex);
        LeaveCriticalSection(&e->mutex);
    }

    // Condition variable.
    ConditionVariable alloc_condition_var()
    {
        Win32BackendData* data = win32_data();
        Win32Entity* e = win32_condition_var_alloc(data);
        return os_condition_var(e);
    }

    void release_condition_var(ConditionVariable cv)
    {
        Win32BackendData* data = win32_data();
        release_win32_condition_var(data, cv);
    }

    bool wait_condition_var(ConditionVariable cv, Mutex mutex, MicroSec end_us)
    {
        uint32_t sleep_ms = win32_sleep_ms_from_end_us(end_us);
        BOOL result = FALSE;
        if (sleep_ms > 0)
        {
            Win32Entity* w32_cv = win32_condition_var(cv);
            Win32Entity* w32_mutex = win32_mutex(mutex);
            result = SleepConditionVariableCS(&w32_cv->cv, &w32_mutex->mutex, sleep_ms);
        }
        return result;
    }

    void notify_one_condition_var(ConditionVariable cv)
    {
        Win32Entity* e = win32_condition_var(cv);
        WakeConditionVariable(&e->cv);
    }

    void notify_all_condition_var(ConditionVariable cv)
    {
        Win32Entity* e = win32_condition_var(cv);
        WakeAllConditionVariable(&e->cv);
    }

    // Setup for core rendering.
    void populate_core_render_data(RenderCoreData* rd_data)
    {
        Win32BackendData* data = win32_data();
        data->render_data = rd_data;
    }
} // namespace OS

// Platform implementation entry-point.
#ifdef WIN32_GFX
int wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    SetUnhandledExceptionFilter(&OS::exception_filter);

    OS::Win32BackendData* data = OS::win32_data();
    if (not OS::init_win32_base(data))
        return 1;

    // Setup for platform.
    const int argc = __argc;

    // Create a list for the utf8-converted arguments.
    String8List string_pool_lst{};
    char** argv = Arena::push_array<char*>(data->win32_arena, static_cast<uint64_t>(argc));

    wchar_t** wargv = __wargv;

    for (int i = 0; i < argc; ++i)
    {
        String8 s = OS::convert_utf16_to_utf8(data->win32_arena, str16_cstr(wargv[i]));
        str8_list_push(data->win32_arena, &string_pool_lst, s);
        argv[i] = s.str;
    }

    // Populate the process environment as well.
    {
        WCHAR* base_block = GetEnvironmentStringsW();
        // https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentstringsw#remarks
        // We're looking for strings until we hit a double '\0'.
        WCHAR* env_block = base_block;
        WCHAR* start = env_block;
        for (; ;++env_block)
        {
            if (*env_block == L'\0')
            {
                // If the last tail move was also to the null byte, we're done.
                if (env_block == start)
                    break;
                String16 wstr = str16(start, static_cast<size_t>(env_block - start));
                String8 s = OS::convert_utf16_to_utf8(data->win32_arena, wstr);
                str8_list_push(data->win32_arena, &data->environment, s);
                // Move 'start' to one past the block pointer so that it will start at the next potential environment
                // variable candidate.
                start = env_block + 1;
            }
        }
        FreeEnvironmentStringsW(base_block);
    }
    return gap_main_entry(argc, argv);
}
#else // ^^^ WIN32_GFX ^^^ // vvv !WIN32_GFX vvv

int wmain(int argc, wchar_t **)
{
    SetUnhandledExceptionFilter(&OS::exception_filter);

    OS::Win32BackendData* data = OS::win32_data();
    if (not OS::init_win32_base(data))
        return 1;

    // Create a list for the utf8-converted arguments.
    String8List string_pool_lst{};
    char** argv = Arena::push_array<char*>(data->win32_arena, static_cast<uint64_t>(argc));

    wchar_t** wargv = __wargv;

    for (int i = 0; i < argc; ++i)
    {
        String8 s = OS::convert_utf16_to_utf8(data->win32_arena, str16_cstr(wargv[i]));
        str8_list_push(data->win32_arena, &string_pool_lst, s);
        argv[i] = s.str;
    }

    // Populate the process environment as well.
    {
        WCHAR* base_block = GetEnvironmentStringsW();
        // https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentstringsw#remarks
        // We're looking for strings until we hit a double '\0'.
        WCHAR* env_block = base_block;
        WCHAR* start = env_block;
        for (; ;++env_block)
        {
            if (*env_block == L'\0')
            {
                // If the last tail move was also to the null byte, we're done.
                if (env_block == start)
                    break;
                String16 wstr = str16(start, static_cast<size_t>(env_block - start));
                String8 s = OS::convert_utf16_to_utf8(data->win32_arena, wstr);
                str8_list_push(data->win32_arena, &data->environment, s);
                // Move 'start' to one past the block pointer so that it will start at the next potential environment
                // variable candidate.
                start = env_block + 1;
            }
        }
        FreeEnvironmentStringsW(base_block);
    }
    return gap_main_entry(argc, argv);
}
#endif // WIN32_GFX