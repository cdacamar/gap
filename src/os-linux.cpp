#include "os.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include <cassert>

#include <forward_list>
#include <string_view>

#include "feed.h"

// HACK!!! The Xrandr extension declares Glyph as a global typedef, but that clashes with
// the Glyph namespace.  Let's just give them something... else.
#define Glyph XrandrGlyph

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/extensions/sync.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <X11/keysymdef.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "gap-core.h"
#include "list-helpers.h"
#include "thread-ctx.h"
#include "utf-8.h"
#include "util.h"

namespace OS
{
    namespace
    {
        constexpr int internal_path_max = PATH_MAX;

        constexpr int pipe_buffer_size = KB(4);

        // Borrowed from sokol_app.h.  The XDND_VERSION.
        constexpr int supported_xdnd_version = 5;

        struct LinuxEntity;

        struct IOPipeEndpoint
        {
            int read_pipe;
            char buf[pipe_buffer_size];
        };

        struct IOPipeData
        {
            int exit_code;
            int child_pid;
            IOPipeEndpoint stdout_ep;
            IOPipeEndpoint stderr_ep;
        };

        using PipeDataAlloc = std::forward_list<IOPipeData>;

        struct AllocPipeDataInput
        {
            int child_pid;
            int stdout_pipe;
            int stderr_pipe;
        };

        struct DirIterData
        {
            DIR* dir;
            dirent* dp;
            std::string core_dir;
            DirIterFlags flags;
        };

        using DirIterDataAlloc = std::forward_list<DirIterData>;

        struct ThreadData
        {
            ThreadEntryPointFunctionType func;
            void* data_p;
            pthread_t handle;
        };

        struct LinuxEntity
        {
            LinuxEntity* next;
            LinuxEntity* prev;
            union
            {
                // Thread.
                ThreadData thread;

                // Mutex.
                pthread_mutex_t mutex;

                // Condition variable.
                pthread_cond_t cv;
            };
        };

        struct LinuxEntityList
        {
            LinuxEntity* first;
            LinuxEntity* last;
            uint64_t count;
        };

        IOPipeData* alloc_pipe_data(PipeDataAlloc* alloc, AllocPipeDataInput in)
        {
            alloc->push_front({});
            IOPipeData* data = &alloc->front();
            data->child_pid = in.child_pid;
            data->stdout_ep.read_pipe = in.stdout_pipe;
            data->stderr_ep.read_pipe = in.stderr_pipe;
            data->exit_code = 0;
            return data;
        }

        void release_pipe_data(PipeDataAlloc* alloc, IOPipeData* data)
        {
            close(data->stdout_ep.read_pipe);
            close(data->stderr_ep.read_pipe);
            ListHelpers::remove_list_element(alloc, data);
        }

        DirIterData* alloc_dir_iter_data(DirIterDataAlloc* alloc)
        {
            alloc->push_front({});
            DirIterData* data = &alloc->front();
            data->dir = nullptr;
            data->dp = nullptr;
            return data;
        }

        void release_dir_iter_data(DirIterDataAlloc* alloc, DirIterData* data)
        {
            if (data->dir != nullptr)
            {
                closedir(data->dir);
            }
            ListHelpers::remove_list_element(alloc, data);
        }

        struct LinuxBackendData
        {
            Display*         display;
            RenderCoreData*  render_data;
            pthread_mutex_t  linux_arena_mutex;
            Arena::Arena*    linux_arena;
            LinuxEntityList  thread_lst;
            LinuxEntityList  mutex_lst;
            LinuxEntityList  cond_var_lst;
            LinuxEntity*     entity_free_list;
            Window           wind;
            Window           xdnd_src_wnd; // For event processing.
            SystemInfo       sys_info;
            Colormap         cmap;
            GLXContext       gl_ctx;
            GLXFBConfig      best_fbc;
            uint64_t         counter;
            std::string      clipboard_data;
            PipeDataAlloc    pipe_data_alloc;
            DirIterDataAlloc dir_iter_data_alloc;
            Ticks            double_click_time;
            ScreenDimensions screen_size;
            ScreenDimensions min_window_size;
            timespec         start_time;
            XID              counter_xid;
            XIM              xim;
            XIC              xic;
            Atom             window_protocols_atom;
            Atom             delete_window_atom;
            Atom             sync_request_atom;
            Atom             sync_request_counter_atom;
            Atom             wm_state_atom;
            Atom             wm_fullscreen_atom;
            // Event atoms.
            Atom             thread_wakeup_atom;
            Atom             clipboard_atom;
            Atom             targets_atom;
            Atom             utf8_string_atom;
            // Xdnd-specific.
            Atom             xdnd_aware;
            Atom             xdnd_enter;
            Atom             xdnd_position;
            Atom             xdnd_status;
            Atom             xdnd_action_copy;
            Atom             xdnd_drop;
            Atom             xdnd_finished;
            Atom             xdnd_selection;
            Atom             xdnd_type_list;
            Atom             xdnd_format_uri;
            // Epoll events.
            Ticks            last_time_step;
            int              epoll;
            int              step_timer_fd;
            int              xdnd_src_version;
            uint32_t         clipboard_sequence;
            Hz               refresh_rate;
            bool             ctx_error_occurred;
            bool             fullscreened;
        };

        LinuxBackendData impl_data;

        LinuxBackendData* linux_data()
        {
            return &impl_data;
        }

        LinuxEntity* push_entity(LinuxBackendData* data, LinuxEntityList* lst)
        {
            LinuxEntity* node = nullptr;
            pthread_mutex_lock(&data->linux_arena_mutex);
            if (data->entity_free_list != nullptr)
            {
                node = data->entity_free_list;
                SLLStackPop(data->entity_free_list);
                zero_bytes(node);
            }
            else
            {
                node = Arena::push_array<LinuxEntity>(data->linux_arena, 1);
            }
            DLLPushBack(lst->first, lst->last, node);
            ++lst->count;
            pthread_mutex_unlock(&data->linux_arena_mutex);
            return node;
        }

        void release_entity(LinuxBackendData* data, LinuxEntityList* lst, LinuxEntity* e)
        {
            pthread_mutex_lock(&data->linux_arena_mutex);
            DLLRemove(lst->first, lst->last, e);
            --lst->count;
            SLLStackPush(data->entity_free_list, e);
            pthread_mutex_unlock(&data->linux_arena_mutex);
        }

        OSWindow os_window(Window wnd)
        {
            return OSWindow(wnd);
        }

        Window linux_window(OSWindow wnd)
        {
            return Window(rep(wnd));
        }

        FileHandle os_file_handle(int fd)
        {
            return FileHandle(fd);
        }

        int linux_file_handle(FileHandle fd)
        {
            return static_cast<int>(rep(fd));
        }

        LibraryHandle os_library_handle(void* so)
        {
            return LibraryHandle{ reinterpret_cast<size_t>(so) };
        }

        void* linux_library_handle(LibraryHandle handle)
        {
            return reinterpret_cast<void*>(rep(handle));
        }

        IOPipe os_pipe_handle(IOPipeData* p)
        {
            return IOPipe{ reinterpret_cast<PrimitiveType<IOPipe>>(p) };
        }

        IOPipeData* linux_pipe_handle(IOPipe h)
        {
            return reinterpret_cast<IOPipeData*>(h);
        }

        DirIter os_dir_iter(DirIterData* d)
        {
            return DirIter{ reinterpret_cast<PrimitiveType<DirIter>>(d) };
        }

        DirIterData* linux_dir_iter(DirIter h)
        {
            return reinterpret_cast<DirIterData*>(h);
        }

        int ctx_error_handler(Display*, XErrorEvent*)
        {
            linux_data()->ctx_error_occurred = true;
            return 0;
        }

        ProcessHandle os_process_handle(pid_t id)
        {
            return static_cast<ProcessHandle>(id);
        }

        pid_t linux_process_handle(ProcessHandle h)
        {
            return static_cast<pid_t>(h);
        }

        // Helper to check for extension string presence.  Adapted from:
        //   http://www.opengl.org/resources/features/OGLextensions/
        bool is_gl_extension_supported(const char* extList, const char* extension)
        {
            const char* start;
            const char* where;
            const char* terminator;
            /* Extension names should not have spaces. */
            where = strchr(extension, ' ');
            if (where != nullptr or *extension == '\0')
                return false;
            // It takes a bit of care to be fool-proof about parsing the
            // OpenGL extensions string. Don't be fooled by sub-strings,
            // etc.
            for (start = extList;;)
            {
                where = strstr(start, extension);

                if (where == nullptr)
                    break;

                terminator = where + strlen(extension);

                if (where == start or *(where - 1) == ' ' )
                {
                    if (*terminator == ' ' or *terminator == '\0' )
                        return true;
                }

                start = terminator;
            }
            return false;
        }

        using PFglXCreateContextAttribsARBProc = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

        bool setup_gl_context(LinuxBackendData* data)
        {
            // Now we get the GLX extensions.
            const char* gl_extensions = glXQueryExtensionsString(data->display, DefaultScreen(data->display));

            // It is not necessary to create or make current to a context before calling glXGetProcAddressARB.
            PFglXCreateContextAttribsARBProc glXCreateContextAttribsARB = nullptr;
            const char* proc_name = "glXCreateContextAttribsARB";
            glXCreateContextAttribsARB = reinterpret_cast<PFglXCreateContextAttribsARBProc>(glXGetProcAddressARB(reinterpret_cast<const unsigned char*>(proc_name)));

            GLXContext ctx = 0;

            // Install an X error handler so the application won't exit if GL 3.0
            // context allocation fails.
            //
            // Note this error handler is global.  All display connections in all threads
            // of a process use the same error handler, so be sure to guard against other
            // threads issuing X commands while this code is running.
            data->ctx_error_occurred = false;
            int (*old_handler)(Display*, XErrorEvent*) = XSetErrorHandler(&ctx_error_handler);

            // Check for the GLX_ARB_create_context extension string and the function.
            // If either is not present, use GLX 1.3 context creation method.
            if (not is_gl_extension_supported(gl_extensions, "GLX_ARB_create_context")
                or glXCreateContextAttribsARB == nullptr)
            {
                fprintf(stderr, "glXCreateContextAttribsARB() not found... using old-style GLX context\n");
                ctx = glXCreateNewContext(data->display, data->best_fbc, GLX_RGBA_TYPE, 0, True);
            }
            // Create the 3.2 context.
            else
            {
                int context_attribs[] =
                {
                    GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                    GLX_CONTEXT_MINOR_VERSION_ARB, 2,
                    //GLX_CONTEXT_FLAGS_ARB        , GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
                    None
                };

                printf( "Creating context\n" );
                ctx = glXCreateContextAttribsARB(data->display, data->best_fbc, 0,
                                                True, context_attribs);

                // Sync to ensure any errors generated are processed.
                XSync(data->display, False);
                if ( !data->ctx_error_occurred and ctx)
                {
#ifndef NDEBUG
                    fprintf(stdout, "Created GL 3.2 context\n");
#endif // NDEBUG
                }
                else
                {
                    // Couldn't create GL 3.2 context.  Fall back to old-style 2.x context.
                    // When a context version below 3.2 is requested, implementations will
                    // return the newest context version compatible with OpenGL versions less
                    // than version 3.0.
                    // GLX_CONTEXT_MAJOR_VERSION_ARB = 1
                    context_attribs[1] = 1;
                    // GLX_CONTEXT_MINOR_VERSION_ARB = 0
                    context_attribs[3] = 0;
                    data->ctx_error_occurred = false;
#ifndef NDEBUG
                    fprintf(stderr, "Failed to create GL 3.0 context... using old-style GLX context\n");
#endif // NDEBUG
                    ctx = glXCreateContextAttribsARB(data->display, data->best_fbc, 0,
                                                        True, context_attribs);
                }
            }
            // Restore the original error handler.
            XSetErrorHandler(old_handler);
            data->gl_ctx = ctx;
            return not data->ctx_error_occurred and data->gl_ctx;
        }

        unsigned long x11_window_property(Window window, Atom property, Atom type, unsigned char** value)
        {
            LinuxBackendData* data = linux_data();
            Atom actual_type;
            int actual_format;
            unsigned long item_count;
            unsigned long bytes_after;
            XGetWindowProperty(data->display,
                            window,
                            property,
                            0,
                            LONG_MAX,
                            False,
                            type,
                            &actual_type,
                            &actual_format,
                            &item_count,
                            &bytes_after,
                            value);
            return item_count;
        }

        Event* push_event(Arena::Arena* arena, Events* lst, EventSort sort, OSWindow window)
        {
            Event* e = Arena::push_array<Event>(arena, 1);
            e->sort = sort;
            e->window = window;
            SLLQueuePush(lst->first, lst->last, e);
            ++lst->count;
            return e;
        }

        Key os_linux_os_key_from_xkey(KeySym ks)
        {
            switch (ks)
            {
            default:
            {
                if(XK_F1 <= ks && ks <= XK_F24)
                    return extend(OS::Key::F1, ks - XK_F1);
                if('0' <= ks && ks <= '9')
                    return extend(OS::Key::_0, ks - '0');
            }
            break;
            case XK_Escape:    return OS::Key::Esc;
            case XK_Control_L: return OS::Key::Ctrl;
            case XK_Control_R: return OS::Key::Ctrl;
            case XK_Shift_L:   return OS::Key::Shift;
            case XK_Shift_R:   return OS::Key::Shift;
            case XK_Alt_L:     return OS::Key::Alt;
            case XK_Alt_R:     return OS::Key::Alt;
            case XK_Up:        return OS::Key::Up;
            case XK_Down:      return OS::Key::Down;
            case XK_Left:      return OS::Key::Left;
            case XK_Right:     return OS::Key::Right;
            case XK_Page_Up:   return OS::Key::PageUp;
            case XK_Page_Down: return OS::Key::PageDown;
            case XK_Home:      return OS::Key::Home;
            case XK_End:       return OS::Key::End;
            case XK_Delete:    return OS::Key::Delete;
            case XK_Return:    return OS::Key::Return;
            case XK_BackSpace: return OS::Key::Backspace;
            case XK_Tab:       return OS::Key::Tab;
            case '-':          return OS::Key::Minus;
            case '=':          return OS::Key::Equal;
            case '[':          return OS::Key::LeftBracket;
            case ']':          return OS::Key::RightBracket;
            case ';':          return OS::Key::Semicolon;
            case '\'':         return OS::Key::Quote;
            case '.':          return OS::Key::Period;
            case ',':          return OS::Key::Comma;
            case '/':          return OS::Key::Slash;
            case '\\':         return OS::Key::BackSlash;
            case '`':          return OS::Key::Tick;
            case 'a':case 'A': return OS::Key::A;
            case 'b':case 'B': return OS::Key::B;
            case 'c':case 'C': return OS::Key::C;
            case 'd':case 'D': return OS::Key::D;
            case 'e':case 'E': return OS::Key::E;
            case 'f':case 'F': return OS::Key::F;
            case 'g':case 'G': return OS::Key::G;
            case 'h':case 'H': return OS::Key::H;
            case 'i':case 'I': return OS::Key::I;
            case 'j':case 'J': return OS::Key::J;
            case 'k':case 'K': return OS::Key::K;
            case 'l':case 'L': return OS::Key::L;
            case 'm':case 'M': return OS::Key::M;
            case 'n':case 'N': return OS::Key::N;
            case 'o':case 'O': return OS::Key::O;
            case 'p':case 'P': return OS::Key::P;
            case 'q':case 'Q': return OS::Key::Q;
            case 'r':case 'R': return OS::Key::R;
            case 's':case 'S': return OS::Key::S;
            case 't':case 'T': return OS::Key::T;
            case 'u':case 'U': return OS::Key::U;
            case 'v':case 'V': return OS::Key::V;
            case 'w':case 'W': return OS::Key::W;
            case 'x':case 'X': return OS::Key::X;
            case 'y':case 'Y': return OS::Key::Y;
            case 'z':case 'Z': return OS::Key::Z;
            case ' ':          return OS::Key::Space;
            }
            return OS::Key::Null;
        }

        bool init_linux(LinuxBackendData* data)
        {
            if (clock_gettime(CLOCK_MONOTONIC_RAW, &data->start_time) != 0)
            {
                fprintf(stderr, "Failed to get start time: clock_gettime failed.\n");
            }
            // Estimate double-click time to be 500ms.
            data->double_click_time = Ticks{ 500 };

            // Epoll.
            data->step_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
            data->epoll = epoll_create(1);

            epoll_event e{ };
            e.events = EPOLLIN;
            e.data.fd = data->step_timer_fd;
            epoll_ctl(data->epoll, EPOLL_CTL_ADD, data->step_timer_fd, &e);
            return true;
        }

        String8 read_symlink(Arena::Arena* arena, String8 lnk)
        {
            ssize_t len = 64;
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            String8 scratch_buf = str8_empty;
            do
            {
                scratch_buf = str8_alloc(scratch.arena, len);

                auto r = readlink(lnk.str, scratch_buf.str, scratch_buf.size);
                // Not a symlink.
                if (r == -1)
                    break;
                if (r < len)
                {
                    // Discard extra space.
                    scratch_buf.size = len;
                    break;
                }
                // Grow geometrically.
                len *= 2;
            } while(true);
            String8 result = str8_copy(arena, scratch_buf);
            Arena::scratch_end(scratch);
            return result;
        }

        void fill_date_time_from_tm(DateTime* out, const tm& in, MicroSec us)
        {
            out->sec  = in.tm_sec;
            out->min  = in.tm_min;
            out->hour = in.tm_hour;
            out->day  = in.tm_mday-1;
            out->mon  = in.tm_mon;
            out->year = in.tm_year+1900;
            out->msec = rep(us);
        }

        void fill_tm_from_date_time(tm* out, const DateTime& in)
        {
            out->tm_sec = in.sec;
            out->tm_min = in.min;
            out->tm_hour= in.hour;
            out->tm_mday= in.day+1;
            out->tm_mon = in.mon;
            out->tm_year= in.year-1900;
        }

        DenseTime dense_time_from_timespec(const timespec& in)
        {
            struct tm tm_time{ };
            gmtime_r(&in.tv_sec, &tm_time);
            DateTime date_time{ };
            fill_date_time_from_tm(&date_time, tm_time, MicroSec(in.tv_nsec / Million(1)));
            return dense_time_from_date_time(date_time);
        }

        bool should_filter_codepoint_from_text(UTF8::Codepoint cp)
        {
            //              and don't allow 'delete' to sneak through.
            return (cp < ' ') or (cp == 0x7F);
        }

        void send_selection_resp(XSelectionEvent* e, Display* disp, Window req)
        {
            XSendEvent(disp, req, True, 0, reinterpret_cast<XEvent*>(e));
        }

        void send_clipboard_data(const XSelectionRequestEvent& req)
        {
            LinuxBackendData* data = linux_data();
            XSelectionEvent resp{ };
            resp.type      = SelectionNotify;
            resp.requestor = req.requestor;
            resp.selection = req.selection;
            resp.target    = req.target;
            resp.time      = req.time;
            resp.property  = None;

            Atom formats[] =
            {
                data->utf8_string_atom,
                XA_STRING,
            };

            if (data->clipboard_data.empty())
            {
                send_selection_resp(&resp, req.display, req.requestor);
                return;
            }

            if (req.selection != data->clipboard_atom or req.property == None)
            {
                send_selection_resp(&resp, req.display, req.requestor);
                return;
            }

            if (req.target == data->targets_atom)
            {
                XChangeProperty(req.display,
                                req.requestor,
                                req.property,
                                XA_ATOM,
                                32,
                                PropModeReplace,
                                reinterpret_cast<unsigned char*>(formats),
                                std::size(formats));
                resp.property = req.property;
            }
            else
            {
                bool prop_found = false;
                for (Atom fmt : formats)
                {
                    if (req.target == fmt)
                    {
                        prop_found = true;
                        break;
                    }
                }

                if (prop_found)
                {
                    XChangeProperty(req.display,
                                    req.requestor,
                                    req.property,
                                    req.target,
                                    8,
                                    PropModeReplace,
                                    reinterpret_cast<unsigned char*>(data->clipboard_data.data()),
                                    static_cast<int>(data->clipboard_data.size()));
                    resp.property = req.property;
                }
            }
            send_selection_resp(&resp, req.display, req.requestor);
        }

        struct PipePair
        {
            int pipes[2]; // r/w.
        };

        PipePair create_pipes()
        {
            PipePair pipe_pair;
            if (pipe(pipe_pair.pipes) == -1)
            {
                pipe_pair.pipes[0] = -1;
                pipe_pair.pipes[1] = -1;
            }
            return pipe_pair;
        }

        bool valid_pipe(PipePair pipe_pair)
        {
            return pipe_pair.pipes[0] != -1
                and pipe_pair.pipes[1] != -1;
        }

        void close_pipes(PipePair pipe_pair)
        {
            if (pipe_pair.pipes[0] != -1)
            {
                close(pipe_pair.pipes[0]);
            }

            if (pipe_pair.pipes[1] != -1)
            {
                close(pipe_pair.pipes[1]);
            }
        }

        // Returns true if a write to the buffer happened.
        bool read_pipe_endpoint(Arena::Arena* arena, String8* out_buf, IOPipeEndpoint* ep)
        {
            ssize_t bytes_read = read(ep->read_pipe, ep->buf, pipe_buffer_size);
            bool close_pipe = false;
            bool buf_written = false;
            if (bytes_read > 0)
            {
                *out_buf = str8_copy(arena, str8(ep->buf, bytes_read));
                buf_written = true;
            }
            else if (bytes_read == -1)
            {
                // If not standard async return, then we should kill this pipe.
                if (errno != EAGAIN)
                {
                    close_pipe = true;
                }
            }
            else
            {
                close_pipe = true;
            }

            if (close_pipe)
            {
                close(ep->read_pipe);
                ep->read_pipe = -1;
            }
            return buf_written;
        }

        bool path_separator(char c)
        {
            return c == '/';
        }

        size_t path_root_end(String8 path)
        {
            // Assume relative, beginning of path.
            if (path.size == 0)
                return 0;
            // Relative path.
            if (not path_separator(path.str[0]))
                return 0;
            // Unix-style path start, e.g. /root/foo.
            return 1;
        }

        Mutex os_mutex(LinuxEntity* e)
        {
            return Mutex{ reinterpret_cast<PrimitiveType<Mutex>>(e) };
        }

        LinuxEntity* linux_mutex(Mutex m)
        {
            return reinterpret_cast<LinuxEntity*>(m);
        }

        LinuxEntity* linux_mutex_alloc(LinuxBackendData* data)
        {
            LinuxEntity* mutex = push_entity(data, &data->mutex_lst);
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            // From man:
            // > pthread_mutex_init always returns 0.
            pthread_mutex_init(&mutex->mutex, &attr);
            pthread_mutexattr_destroy(&attr);
            return mutex;
        }

        void release_linux_mutex(LinuxBackendData* data, Mutex m)
        {
            LinuxEntity* e = linux_mutex(m);
            pthread_mutex_destroy(&e->mutex);
            release_entity(data, &data->mutex_lst, e);
        }

        ConditionVariable os_condition_var(LinuxEntity* e)
        {
            return ConditionVariable{ reinterpret_cast<PrimitiveType<ConditionVariable>>(e) };
        }

        LinuxEntity* linux_condition_var(ConditionVariable m)
        {
            return reinterpret_cast<LinuxEntity*>(m);
        }

        LinuxEntity* linux_condition_var_alloc(LinuxBackendData* data)
        {
            LinuxEntity* cv = push_entity(data, &data->cond_var_lst);
            int init_result = pthread_cond_init(&cv->cv, nullptr);
            if (init_result != 0)
            {
                release_entity(data, &data->cond_var_lst, cv);
                return nullptr;
            }
            return cv;
        }

        void release_linux_condition_var(LinuxBackendData* data, ConditionVariable cv)
        {
            LinuxEntity* e = linux_condition_var(cv);
            pthread_cond_destroy(&e->cv);
            release_entity(data, &data->cond_var_lst, linux_condition_var(cv));
        }

        Thread os_thread(LinuxEntity* e)
        {
            return Thread{ reinterpret_cast<PrimitiveType<Thread>>(e) };
        }

        LinuxEntity* linux_thread(Thread t)
        {
            return reinterpret_cast<LinuxEntity*>(t);
        }

        void* linux_thread_entry_point(void* ptr)
        {
            LinuxEntity* e = static_cast<LinuxEntity*>(ptr);
            thread_entry_bridge(e->thread.func, e->thread.data_p);
            return nullptr;
        }

        LinuxEntity* linux_alloc_thread(LinuxBackendData* data, ThreadEntryPointFunctionType func, void* data_p)
        {
            LinuxEntity* thread = push_entity(data, &data->thread_lst);
            thread->thread.func = func;
            thread->thread.data_p = data_p;
            int result = pthread_create(&thread->thread.handle, nullptr, linux_thread_entry_point, thread);
            if (result != 0)
            {
                release_entity(data, &data->thread_lst, thread);
                thread = nullptr;
            }
            return thread;
        }

        void linux_release_thread(LinuxBackendData* data, Thread thread)
        {
            release_entity(data, &data->thread_lst, linux_thread(thread));
        }

        void x11_process_xdnd_selection(Arena::Arena* arena, const XSelectionEvent& sel, LinuxBackendData* data, Events* lst, OSWindow wnd)
        {
            auto scratch = Arena::scratch_begin({ &arena, 1 });
            unsigned char* evt_data = nullptr;
            uint32_t count = x11_window_property(sel.requestor,
                                                sel.property,
                                                sel.target,
                                                &evt_data);
            fprintf(stdout, "Dropped files: %.*s\n", count, evt_data);
            SplitStringsInput split_in{
                .in = str8(reinterpret_cast<char*>(evt_data), count),
                .seps = str8_mut(str8_literal("\r\n")),
                .flags = {},
            };
            String8List split_result{};
            split_strings(scratch.arena, &split_result, split_in);
            for EachNode(n, split_result.first)
            {
                fprintf(stdout, "split: %.*s\n", static_cast<int>(n->string.size), n->string.str);
            }
            // Now we can start to add our dropped files.
            DropFileList drop_list = {};
            Event* event = push_event(arena, lst, EventSort::FileDrop, wnd);
            {
                // Get the pointer location for this drop.
                // Note: This is a bit of a hack.  What we _should_ do is listen to the accompanying
                // XdndPosition event just before this and use that position.
                LinuxBackendData* data = linux_data();
                Window root_rtn;
                Window child_rtn;
                Vec2i root_pt;
                Vec2i wnd_pt;
                unsigned int mask;
                if (XQueryPointer(data->display,
                                    data->wind,
                                    &root_rtn,
                                    &child_rtn,
                                    &root_pt.x,
                                    &root_pt.y,
                                    &wnd_pt.x,
                                    &wnd_pt.y,
                                    &mask) != 0)
                {
                    event->pos.x = static_cast<float>(wnd_pt.x);
                    event->pos.y = static_cast<float>(wnd_pt.y);
                }
            }
            for EachNode(n, split_result.first)
            {
                // Remove the 'file://' prefix.
                String8 file = str8_chop_prefix(n->string, str8_mut(str8_literal("file://")));
                String8Node* node = Arena::push_array_no_zero<String8Node>(arena, 1);
                node->string = str8_copy(arena, file);
                SLLQueuePush(drop_list.first, drop_list.last, node);
                ++drop_list.count;
            }
            Arena::scratch_end(scratch);
            lst->drop_files = drop_list;
            // Reply that drop was accepted.
            if (data->xdnd_src_version >= 2)
            {
                XEvent reply = {};
                reply.type = ClientMessage;
                reply.xclient.window = data->xdnd_src_wnd;
                reply.xclient.message_type = data->xdnd_finished;
                reply.xclient.format = 32;
                reply.xclient.data.l[0] = static_cast<long>(data->wind);
                reply.xclient.data.l[1] = count;
                reply.xclient.data.l[2] = static_cast<long>(data->xdnd_action_copy);
                XSendEvent(data->display, data->xdnd_src_wnd, False, NoEventMask, &reply);
                XFlush(data->display);
            }
        }

        void x11_recv_clipboard_data(const XSelectionEvent& sel, LinuxBackendData* data)
        {
            Atom type{};
            int fmt = 0;
            unsigned long count = 0;
            unsigned long overflow = 0;
            unsigned char* clip_data = nullptr;
            int result = XGetWindowProperty(data->display,
                                            sel.requestor,
                                            sel.property,
                                            0L, LONG_MAX, True,
                                            data->utf8_string_atom, &type, &fmt, &count,
                                            &overflow, &clip_data);
            if (result != Success)
            {
                if (clip_data != nullptr)
                {
                    XFree(clip_data);
                }
                return;
            }
            if (fmt != 8)
            {
                if (clip_data != nullptr)
                {
                    XFree(clip_data);
                }
                return;
            }
            data->clipboard_data.clear();
            data->clipboard_data.assign(reinterpret_cast<char*>(clip_data), count);
            XFree(clip_data);
            XDeleteProperty(data->display, data->wind, data->clipboard_atom);
        }

        void x11_process_selection_event(Arena::Arena* arena, LinuxBackendData* data, const XSelectionEvent& sel, Events* lst, OSWindow wnd)
        {
            if (sel.selection == data->xdnd_selection)
            {
                x11_process_xdnd_selection(arena, sel, data, lst, wnd);
            }
            else if (sel.selection == data->clipboard_atom
                    and sel.target == data->utf8_string_atom
                    and sel.property == None)
            {
                x11_recv_clipboard_data(sel, data);
            }
        }
    } // namespace [anon]

    // --- START GAP API ---
    // Windowing.
    OSWindow init_window(Vec4i wind_rect, String8 title)
    {
        // Largely borrowed from: https://www.khronos.org/opengl/wiki/Tutorial:_OpenGL_3.0_Context_Creation_(GLX)
        Display* display = XOpenDisplay(nullptr);

        if (display == nullptr)
        {
            fprintf(stderr, "Unable to get display reference: XOpenDisplay\n");
            return OSWindow::Sentinel;
        }

        // Fill in display atoms.
        LinuxBackendData* data = linux_data();
        data->display                   = display;
        data->window_protocols_atom     = XInternAtom(data->display, "WM_PROTOCOLS", False);
        data->delete_window_atom        = XInternAtom(data->display, "WM_DELETE_WINDOW", False);
        data->sync_request_atom         = XInternAtom(data->display, "_NET_WM_SYNC_REQUEST", False);
        data->sync_request_counter_atom = XInternAtom(data->display, "_NET_WM_SYNC_REQUEST_COUNTER", False);
        data->wm_state_atom             = XInternAtom(data->display, "_NET_WM_STATE", False);
        data->wm_fullscreen_atom        = XInternAtom(data->display, "_NET_WM_STATE_FULLSCREEN", False);
        data->thread_wakeup_atom        = XInternAtom(data->display, "_GAP_WAKEUP", False);
        data->clipboard_atom            = XInternAtom(data->display, "CLIPBOARD", False);
        data->targets_atom              = XInternAtom(data->display, "TARGETS", False);
        data->utf8_string_atom          = XInternAtom(data->display, "UTF8_STRING", False);
        // For drag-and-drop.
        data->xdnd_aware       = XInternAtom(data->display, "XdndAware", False);
        data->xdnd_enter       = XInternAtom(data->display, "XdndEnter", False);
        data->xdnd_position    = XInternAtom(data->display, "XdndPosition", False);
        data->xdnd_status      = XInternAtom(data->display, "XdndStatus", False);
        data->xdnd_action_copy = XInternAtom(data->display, "XdndActionCopy", False);
        data->xdnd_drop        = XInternAtom(data->display, "XdndDrop", False);
        data->xdnd_finished    = XInternAtom(data->display, "XdndFinished", False);
        data->xdnd_selection   = XInternAtom(data->display, "XdndSelection", False);
        data->xdnd_type_list   = XInternAtom(data->display, "XdndTypeList", False);
        data->xdnd_format_uri  = XInternAtom(data->display, "text/uri-list", False);

        data->screen_size = ScreenDimensions{ Width{ wind_rect.z }, Height{ wind_rect.a } };

        int visual_attrs[] =
        {
            GLX_X_RENDERABLE    , True,
            GLX_DRAWABLE_TYPE   , GLX_WINDOW_BIT,
            GLX_RENDER_TYPE     , GLX_RGBA_BIT,
            GLX_X_VISUAL_TYPE   , GLX_TRUE_COLOR,
            GLX_RED_SIZE        , 8,
            GLX_GREEN_SIZE      , 8,
            GLX_BLUE_SIZE       , 8,
            GLX_ALPHA_SIZE      , 8,
            GLX_DEPTH_SIZE      , 24,
            GLX_STENCIL_SIZE    , 8,
            GLX_DOUBLEBUFFER    , True,
            //GLX_SAMPLE_BUFFERS  , 1,
            //GLX_SAMPLES         , 4,
            None
        };

        int glx_major = 0;
        int glx_minor = 0;
        // FBConfigs were added in GLX version 1.3.
        if (not glXQueryVersion(display, &glx_major, &glx_minor)
            or ((glx_major == 1) and (glx_minor < 3))
            or (glx_major < 1))
        {
            fprintf(stderr, "Invalid GLX version: '%d.%d'\n", glx_major, glx_minor);
            return OSWindow::Sentinel;
        }

        int fbcount = 0;
        GLXFBConfig* fbc = glXChooseFBConfig(display, DefaultScreen(display), visual_attrs, &fbcount);
        if (fbc == nullptr)
        {
            fprintf(stderr, "Failed to retrieve framebuffer config\n");
            return OSWindow::Sentinel;
        }

        // Pick the FB config/visual with the most samples per pixel.
        int best_fbc = -1;
        int worst_fbc = -1;
        int best_num_samp = -1;
        int worst_num_samp = 999;
        for (int i = 0; i < fbcount; ++i)
        {
            XVisualInfo *vi = glXGetVisualFromFBConfig(display, fbc[i]);
            if (vi != nullptr)
            {
                int samp_buf = 0;
                int samples = 0;
                glXGetFBConfigAttrib( display, fbc[i], GLX_SAMPLE_BUFFERS, &samp_buf );
                glXGetFBConfigAttrib( display, fbc[i], GLX_SAMPLES       , &samples  );
#ifndef NDEBUG
                fprintf(stdout, "Matching fbconfig %d, visual ID 0x%2x: SAMPLE_BUFFERS = %d, SAMPLES = %d\n",
                        i, static_cast<unsigned>(vi->visualid), samp_buf, samples);
#endif // NDEBUG
                if ((best_fbc < 0 or samp_buf) and samples > best_num_samp)
                {
                    best_fbc = i;
                    best_num_samp = samples;
                }

                if ((worst_fbc < 0 or not samp_buf) or samples < worst_num_samp)
                {
                    worst_fbc = i;
                    worst_num_samp = samples;
                }
            }
            XFree(vi);
        }

        data->best_fbc = fbc[best_fbc];

        // Be sure to free the FBConfig list allocated by glXChooseFBConfig()
        XFree(fbc);

        XVisualInfo* vi = glXGetVisualFromFBConfig(display, data->best_fbc);
#ifndef NDEBUG
        fprintf(stdout, "Chose visual ID = 0x%x\n", static_cast<unsigned>(vi->visualid));
#endif // NDEBUG

        XSetWindowAttributes wnd_attrs{};
        data->cmap = XCreateColormap(display,
                        RootWindow(display, vi->screen),
                        vi->visual, AllocNone);
        wnd_attrs.colormap          = data->cmap;
        wnd_attrs.background_pixmap = None;
        wnd_attrs.border_pixel      = 0;
        wnd_attrs.event_mask        = StructureNotifyMask;

        wind_rect.x = wind_rect.x == default_window_pos.x ? 0 : wind_rect.x;
        wind_rect.y = wind_rect.y == default_window_pos.y ? 0 : wind_rect.y;

        Window wind = XCreateWindow(display,
                                    RootWindow(display, vi->screen),
                                    wind_rect.x, wind_rect.y,
                                    wind_rect.z, wind_rect.a,
                                    0,
                                    vi->depth,
                                    InputOutput,
                                    vi->visual,
                                    CWBorderPixel | CWColormap | CWEventMask,
                                    &wnd_attrs);
        if (not wind)
        {
            fprintf(stderr, "Failed to create window\n");
            return OSWindow::Sentinel;
        }
        data->wind = wind;

        // Equip with X11 input info.
        XSelectInput(display, wind,
                    ExposureMask
                    | PointerMotionMask
                    | ButtonPressMask
                    | ButtonReleaseMask
                    | KeyPressMask
                    | KeyReleaseMask
                    | FocusChangeMask
                    | StructureNotifyMask);
        // Protocols.
        Atom protocols[] =
        {
            data->delete_window_atom,
            data->sync_request_atom
        };
        XSetWMProtocols(display, wind, protocols, std::size(protocols));
        {
            XSyncValue initial{};
            XSyncIntToValue(&initial, 0);
            data->counter_xid = XSyncCreateCounter(display, initial);
        }
        XChangeProperty(display,
                        wind,
                        data->sync_request_counter_atom,
                        XA_CARDINAL,
                        32,
                        PropModeReplace,
                        reinterpret_cast<unsigned char*>(&data->counter_xid),
                        1);

        // To enable drag-and-drop.
        {
            Atom xdnd_version = supported_xdnd_version;
            XChangeProperty(display,
                            wind,
                            data->xdnd_aware,
                            XA_ATOM,
                            32,
                            PropModeReplace,
                            reinterpret_cast<unsigned char*>(&xdnd_version),
                            1);
        }

        // Done with the temp visual data.
        XFree(vi);
        XStoreName(display, wind, title.str);
        XMapWindow(display, wind);

        // More input processing.
        data->xim = XOpenIM(data->display, nullptr, nullptr, nullptr);
        if (not data->xim)
        {
            // Fallback to internal input method.
            XSetLocaleModifiers("@im=none");
            data->xim = XOpenIM(data->display, nullptr, nullptr, nullptr);
        }

        // Find a supported input style.
        XIMStyles* styles = nullptr;
        const XIMStyle style_want = (XIMPreeditNothing | XIMStatusNothing);
        bool found_style = false;
        if (XGetIMValues(data->xim, XNQueryInputStyle, &styles, nullptr) == nullptr
            and styles != nullptr)
        {
            for (uint32_t i = 0; i < styles->count_styles; ++i)
            {
                const XIMStyle style = styles->supported_styles[i];
                if (style == style_want)
                {
                    found_style = true;
                    break;
                }
            }
        }

        if (not found_style)
        {
            fprintf(stderr, "Unable to find desired XIM style.\n");
        }
        data->xic = XCreateIC(data->xim,
                                XNInputStyle, style_want,
                                XNClientWindow, data->wind,
                                XNFocusWindow, data->wind,
                                nullptr);
        if (not setup_gl_context(data))
        {
            fprintf(stderr, "Failed to create OpenGL context.\n");
            return OSWindow::Sentinel;
        }

#ifndef NDEBUG
        if (not glXIsDirect(data->display, data->gl_ctx))
        {
            fprintf(stdout, "Indirect GLX rendering context obtained.\n");
        }
        else
        {
            fprintf(stdout, "Direct GLX rendering context obtained.\n");
        }
#endif // NDEBUG
        glXMakeCurrent(data->display, data->wind, data->gl_ctx);
        // Setup the refresh rate.
        data->refresh_rate = recompute_monitor_refresh_rate();
        return os_window(data->wind);
    }

// Undef a few macros from Linux headers.
#undef None
#undef Success

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
            return str8_mut(str8_literal("Failed to get invoke realpath"));
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
        return OSError{ errno };
    }

    String8 format_error(Arena::Arena* arena, OSError err)
    {
        String8 result{};
        String8 os_err_txt = str8_cstr(strerror(rep(err)));
        result = str8_copy(arena, os_err_txt);
        return result;
    }

    void window_minimum_size(ScreenDimensions min_size)
    {
        LinuxBackendData* data = linux_data();
        data->min_window_size = min_size;

        XSizeHints* size_hints = XAllocSizeHints();
        size_hints->flags = PMinSize;
        size_hints->min_width = rep(min_size.width);
        size_hints->min_height = rep(min_size.height);
        XSetWMNormalHints(data->display, data->wind, size_hints);
        XFree(size_hints);
    }

    void destroy_window(OSWindow wind)
    {
        LinuxBackendData* data = linux_data();
        XDestroyWindow(data->display, data->wind);
        XFreeColormap(data->display, data->cmap);
        XCloseDisplay(data->display);
    }

    bool window_minimized(OSWindow)
    {
        // NYI.
        return false;
    }

    bool window_maximized(OSWindow)
    {
        // NYI.
        return false;
    }

    void window_maximize(OSWindow)
    {
        // NYI.
    }

    void window_minimize(OSWindow)
    {
        // NYI.
    }

    void window_restore(OSWindow)
    {
        // NYI.
    }

    bool window_fullscreened(OSWindow)
    {
        LinuxBackendData* data = linux_data();
        return data->fullscreened;
    }

    void window_fullscreen(OSWindow wind)
    {
        LinuxBackendData* data = linux_data();
        data->fullscreened = true;

        XEvent event{ };
        event.type = ClientMessage;
        event.xclient.serial = 0;
        event.xclient.send_event = True;
        event.xclient.display = data->display;
        event.xclient.window = linux_window(wind);
        event.xclient.message_type = data->wm_state_atom;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 1;
        event.xclient.data.l[1] = data->wm_fullscreen_atom;
        event.xclient.data.l[2] = 0;
        event.xclient.data.l[3] = 0;
        event.xclient.data.l[4] = 0;

        XSendEvent(data->display, DefaultRootWindow(data->display), False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
        XFlush(data->display);
    }

    void window_windowed(OSWindow wind)
    {
        LinuxBackendData* data = linux_data();
        data->fullscreened = false;

        XEvent event{ };
        event.type = ClientMessage;
        event.xclient.serial = 0;
        event.xclient.send_event = True;
        event.xclient.display = data->display;
        event.xclient.window = linux_window(wind);
        event.xclient.message_type = data->wm_state_atom;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 0;
        event.xclient.data.l[1] = data->wm_fullscreen_atom;
        event.xclient.data.l[2] = 0;
        event.xclient.data.l[3] = 0;
        event.xclient.data.l[4] = 0;

        XSendEvent(data->display, DefaultRootWindow(data->display), False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
        XFlush(data->display);
    }

    Vec4i window_rect(OSWindow wind)
    {
        LinuxBackendData* data = linux_data();
        XWindowAttributes attrs = {};
        XGetWindowAttributes(data->display, data->wind, &attrs);
        return { attrs.x, attrs.y, attrs.width, attrs.height };
    }

    void swap_buffers(OSWindow wind)
    {
        LinuxBackendData* data = linux_data();
        glXSwapBuffers(data->display, data->wind);

        // Now we setup the frame timer based on the current Hz
        // and wait to flip the next buffer.  This is because we
        // cannot reliably get the OpenGL extension which controls
        // v-sync behavior.  We determine the monitor refresh rate
        // independently.
        // Start the timer.
        // We need a diff so we account for time lost rendering.
        Ticks now = get_ticks();
        Ticks diff = Ticks{ rep(now) - rep(data->last_time_step) };
        data->last_time_step = now;
        Ticks target = Ticks(Thousand(1) / rep(monitor_refresh_rate()));
        itimerspec ts{ };
        timerfd_gettime(data->step_timer_fd, &ts);
        if (diff > target)
        {
            ts.it_value.tv_nsec = 1;
            timerfd_settime(data->step_timer_fd, 0, &ts, nullptr);
        }
        else
        {
            if (ts.it_value.tv_sec == 0 and ts.it_value.tv_nsec == 0)
            {
                // Note: We add one so that the timer will actually trigger:
                // new_value.it_value specifies the initial expiration of the timer, in seconds and nanoseconds.  Setting either
                // field of new_value.it_value to a nonzero value arms the timer.  Setting both fields of new_value.it_value to
                // zero disarms the timer.
                ts.it_value.tv_nsec = 1 + (rep(target) - rep(diff)) * Million(1);
                timerfd_settime(data->step_timer_fd, 0, &ts, nullptr);
            }
        }
        epoll_event event;
        // Note: -1 == infinite timeout.
        int num_events = epoll_wait(data->epoll, &event, 1, -1);
        if (num_events == 1)
        {
            if (event.data.fd == data->step_timer_fd)
            {
                // Read the actual timer.
                uint64_t count;
                int ret = 0;
                do
                {
                    ret = read(data->step_timer_fd, &count, sizeof count);
                } while(ret != -1 or errno != EAGAIN);
            }
        }
    }

    OSWindow core_window()
    {
        return os_window(linux_data()->wind);
    }

    Error apply_window_border_color(OSWindow, const Vec4f&)
    {
        // NYI.
        return Error::None;
    }

    Error apply_title_font_color(OSWindow, const Vec4f&)
    {
        // NYI.
        return Error::None;
    }

#ifdef LINUX_GFX
    // Event processing.
    // Note: This API will append more events to 'lst'.
    void query_events(Arena::Arena* arena, Events* lst, Wait wait)
    {
        LinuxBackendData* data = linux_data();
        // If we already have events queued up, let's try to get some more.
        if (lst->count != 0)
        {
            wait = Wait::No;
        }

        for (; XPending(data->display) > 0 or (is_yes(wait) and lst->count == 0);)
        {
            XEvent evt{ };
            XNextEvent(data->display, &evt);
            OSWindow wnd = os_window(evt.xclient.window);
            bool release = false;
            switch (evt.type)
            {
            case KeyRelease:
                {
                    release = 1;
                }
            case KeyPress:
                {
                    // This was helpful in seeing how these should be filtered into text events.
                    // https://gist.github.com/baines/5a49f1334281b2685af5dcae81a6fa8a.
                    KeySym ks = XLookupKeysym(&evt.xkey, 0);
                    // There's a special provision here.  If we get a key release and that key is queued again (e.g. the user is holding it down)
                    // we do NOT want to send an event, otherwise this will cause the editor not to batch events, causing frame time issues.
                    if (release)
                    {
                        XEvent dummy;
                        struct CheckKeyData
                        {
                            XEvent* event;
                            bool found;
                        };

                        CheckKeyData check_data{
                            .event = &evt,
                            .found = false
                        };

                        constexpr auto checker = [](Display* disp, XEvent* next_event, XPointer arg)
                        {
                            CheckKeyData* data = reinterpret_cast<CheckKeyData*>(arg);
                            if (next_event->type == KeyPress
                                and next_event->xkey.keycode == data->event->xkey.keycode
                                and next_event->xkey.time - data->event->xkey.time < 2)
                            {
                                data->found = true;
                            }
                            return False;
                        };

                        if (XPending(data->display))
                        {
                            XCheckIfEvent(data->display, &dummy, checker, reinterpret_cast<XPointer>(&check_data));
                            if (check_data.found)
                                continue;
                        }
                    }
                    Event* event = push_event(arena, lst, release ? EventSort::Release : EventSort::Press, wnd);
                    uint32_t x11_mods = evt.xkey.state;
                    if (x11_mods & ShiftMask)
                    {
                        event->modifiers |= KeyMods::Shift;
                    }

                    if (x11_mods & ControlMask)
                    {
                        event->modifiers |= KeyMods::Ctrl;
                    }

                    if (x11_mods & Mod1Mask)
                    {
                        event->modifiers |= KeyMods::Alt;
                    }
                    if (x11_mods & Mod4Mask)
                    {
                        // SUPER.
                    }
                    if (x11_mods & Button1Mask)
                    {
                        // LMB.
                    }
                    if (x11_mods & Button2Mask)
                    {
                        // MMB.
                    }
                    if (x11_mods & Button3Mask)
                    {
                        // RMB.
                    }
                    event->key = os_linux_os_key_from_xkey(ks);
                    event->repeat_count = 0;
                    event->right_sided = false;
                    // Text events can also happen as the result of a specific key down.
                    if (not XFilterEvent(&evt, 0) and (event->modifiers & ~KeyMods::Shift) == KeyMods::None)
                    {
                        char text[32]{};
                        Status status{};
                        int len = Xutf8LookupString(data->xic, &evt.xkey, text, std::size(text) - 1, &ks, &status);
                        if (status == XBufferOverflow)
                        {
                            // An IME was probably used and wants to commit more than 32 chars.
                            // Ignore for now.
                        }

                        if ((status == XLookupChars or status == XLookupBoth)
                            and len > 0)
                        {
                            // Start splatting UTF32 chars.
                            UTF8::CodepointWalker walker{ { text, static_cast<size_t>(len) } };
                            while (not walker.exhausted())
                            {
                                UTF8::Codepoint cp = walker.next();
                                if (should_filter_codepoint_from_text(cp))
                                    continue;
                                Event* txt_event = push_event(arena, lst, EventSort::Text, wnd);
                                txt_event->character = cp;
                            }
                        }
                    }
                }
                break;
            case ButtonRelease:
                {
                    Event* event = push_event(arena, lst, EventSort::Release, wnd);
                    switch (evt.xbutton.button)
                    {
                    case Button1:
                        event->key = Key::LeftMouseButton;
                        break;
                    case Button2:
                        event->key = Key::MiddleMouseButton;
                        break;
                    case Button3:
                        event->key = Key::RightMouseButton;
                        break;
                    }
                    event->pos.x = static_cast<float>(evt.xbutton.x);
                    event->pos.y = static_cast<float>(evt.xbutton.y);
                }
                break;
            case ButtonPress:
                {
                    Event* event = push_event(arena, lst, EventSort::Press, wnd);
                    switch (evt.xbutton.button)
                    {
                    case Button1:
                        event->key = Key::LeftMouseButton;
                        break;
                    case Button2:
                        event->key = Key::MiddleMouseButton;
                        break;
                    case Button3:
                        event->key = Key::RightMouseButton;
                        break;
                    case Button4:
                        {
                            // Convert to a scroll event.
                            event->sort = EventSort::Scroll;
                            float wheel_delta_amount = 1.f;
                            event->wheel_delta = Vec2f{ 0.f, wheel_delta_amount };
                        }
                        break;
                    case Button5:
                        {
                            // Convert to a scroll event.
                            event->sort = EventSort::Scroll;
                            float wheel_delta_amount = -1.f;
                            event->wheel_delta = Vec2f{ 0.f, wheel_delta_amount };
                        }
                        break;
// For horizontal scrolling.
#ifndef Button6
#define Button6 6
#endif
#ifndef Button7
#define Button7 7
#endif
                    case Button6:
                        {
                            // Convert to a scroll event.
                            event->sort = EventSort::Scroll;
                            float wheel_delta_amount = -1.f;
                            event->wheel_delta = Vec2f{ wheel_delta_amount, 0.f };
                        }
                        break;
                    case Button7:
                        {
                            // Convert to a scroll event.
                            event->sort = EventSort::Scroll;
                            float wheel_delta_amount = 1.f;
                            event->wheel_delta = Vec2f{ wheel_delta_amount, 0.f };
                        }
                        break;
                    }
                    event->pos.x = static_cast<float>(evt.xbutton.x);
                    event->pos.y = static_cast<float>(evt.xbutton.y);
                }
                break;
            case MotionNotify:
                {
                    Event* event = push_event(arena, lst, EventSort::MouseMove, wnd);
                    event->pos.x = static_cast<float>(evt.xmotion.x);
                    event->pos.y = static_cast<float>(evt.xmotion.y);
                }
                break;
            case PropertyNotify:
                if (evt.xproperty.atom == data->wm_state_atom)
                {
                    // Just a workaround until I can figure out what is going on with this event.
                    XPropertyEvent* prop_event = &evt.xproperty;
                    if (prop_event->state == PropertyNewValue)
                    {
                        push_event(arena, lst, EventSort::GapThreadWakeup, wnd);
                    }
                }
                break;
            case FocusOut:
                {
                    push_event(arena, lst, EventSort::FocusLost, wnd);
                }
                break;
            case SelectionRequest:
                {
                    send_clipboard_data(evt.xselectionrequest);
                }
                break;
            case SelectionNotify:
                {
                    x11_process_selection_event(arena, data, evt.xselection, lst, wnd);
                }
                break;
            case ConfigureNotify:
                {
                    int32_t w = evt.xconfigure.width;
                    int32_t h = evt.xconfigure.height;
                    data->screen_size = ScreenDimensions{ Width{ w }, Height{ h } };
                    if (data->render_data != nullptr and not window_minimized(wnd))
                    {
                        update_frame(data->render_data);
                    }
                }
                break;
            case ClientMessage:
                {
                    if (evt.xclient.message_type == data->window_protocols_atom)
                    {
                        const Atom protocol = static_cast<Atom>(evt.xclient.data.l[0]);
                        if (protocol == data->delete_window_atom)
                        {
                            push_event(arena, lst, EventSort::Quit, wnd);
                        }
                    }
                    else if (evt.xclient.message_type == data->sync_request_atom)
                    {
                        data->counter = 0;
                        data->counter |= evt.xclient.data.l[2];
                        data->counter |= (evt.xclient.data.l[3] << 32);
                        XSyncValue value{};
                        XSyncIntToValue(&value, data->counter);
                        XSyncSetCounter(data->display, data->counter_xid, value);
                    }
                    else if (evt.xclient.message_type == data->thread_wakeup_atom)
                    {
                        push_event(arena, lst, EventSort::GapThreadWakeup, wnd);
                    }
                    else if (evt.xclient.message_type == data->xdnd_drop)
                    {
                        // Note: We should probably check to see if the target format is supported first.
                        // Convert to selection.
                        XConvertSelection(data->display,
                                            data->xdnd_selection,
                                            data->xdnd_format_uri,
                                            data->xdnd_selection,
                                            data->wind,
                                            CurrentTime);
                    }
                    else if (evt.xclient.message_type == data->xdnd_enter)
                    {
                        data->xdnd_src_wnd = static_cast<Window>(evt.xclient.data.l[0]);
                        // https://www.accum.se/~vatten/XDND.html
                        // Bit 0 is set if the source supports more than three data types.
                        // The high byte contains the protocol version to use (minimum of the source's and target's highest supported versions).
                        // The rest of the bits are reserved for future use.
                        data->xdnd_src_version = evt.xclient.data.l[1] >> 24;
                    }
                    else if (evt.xclient.message_type == data->xdnd_position)
                    {
                        if (data->xdnd_src_version != supported_xdnd_version)
                        {
                            auto scratch = Arena::scratch_begin(Arena::no_conflicts);
                            String8 msg = str8_fmt(scratch.arena, "Possible unsupported Xdnd version: %d.  Supported version is %d.", data->xdnd_src_version, supported_xdnd_version);
                            Feed::global_feed()->queue_info(msg);
                            Arena::scratch_end(scratch);
                        }
                        // Now we can notify to accept the drop.
                        XEvent reply = {};
                        reply.type = ClientMessage;
                        reply.xclient.window = data->xdnd_src_wnd;
                        reply.xclient.message_type = data->xdnd_status;
                        reply.xclient.format = 32;
                        reply.xclient.data.l[0] = static_cast<long>(data->wind);

                        reply.xclient.data.l[1] = 1; // Accept with no rectangle.
                        if (data->xdnd_src_version >= 2)
                        {
                            reply.xclient.data.l[4] = data->xdnd_action_copy;
                        }
                        XSendEvent(data->display, data->xdnd_src_wnd, False, NoEventMask, &reply);
                        XFlush(data->display);
                    }
                }
                break;
            }
        }
    }
#endif // LINUX_GFX

    // Direct Interaction.
    // Mouse cursor.
    void set_cursor(CursorStyle)
    {
        // NYI.
    }

    bool delta_meets_double_click_time(Ticks start, Ticks end)
    {
        if (start > end)
            return false;
        auto double_click_time = linux_data()->double_click_time;
        return ((rep(end) - rep(start)) <= rep(double_click_time));
    }

    bool delta_meets_double_click_time(Ticks32 start, Ticks32 end)
    {
        if (start > end)
            return false;
        auto double_click_time = linux_data()->double_click_time;
        return ((rep(end) - rep(start)) <= rep(double_click_time));
    }

    // Clipboard.
    Error clipboard_text(Arena::Arena* arena, String8* result)
    {
        LinuxBackendData* data = linux_data();
        if (XGetSelectionOwner(data->display, data->clipboard_atom) != data->wind)
        {
            data->clipboard_data.clear();
            Atom none = XInternAtom(data->display, "GAP_SELECTION", False);
            // Cribbed from sokil_app.h.
            XConvertSelection(data->display,
                                data->clipboard_atom,
                                data->utf8_string_atom,
                                none,
                                data->wind,
                                CurrentTime);
            XEvent event = {};
            MicroSec start = now_microseconds();
            constexpr uint64_t timeout = Million(100);
            bool usable = true;
            while (not XCheckTypedWindowEvent(data->display, data->wind, SelectionNotify, &event))
            {
                pollfd fd = {
                    .fd = ConnectionNumber(data->display),
                    .events = POLLIN,
                    .revents = 0 };
                poll(&fd, 1, Thousand(1)); // 1 Second wait.
                if ((rep(now_microseconds()) - rep(start)) > timeout)
                {
                    usable = false;
                    break;
                }
            }

            usable &= event.xselection.property != 0;
            if (usable)
            {
                x11_recv_clipboard_data(event.xselection, data);
            }
        }
        *result = str8_copy(arena, str8_cppview(data->clipboard_data));
        return Error::None;
    }

    Error set_clipboard(String8 buf)
    {
        LinuxBackendData* data = linux_data();
        ++data->clipboard_sequence;
        data->clipboard_data = sv_str8(buf);
        XSetSelectionOwner(data->display, data->clipboard_atom, data->wind, CurrentTime);
        return Error::None;
    }

    Error set_clipboard_html(String8 buf, String8 html)
    {
        // HTML support NYI.
        return set_clipboard(buf);
    }

    ClipboardIdentity clipboard_id()
    {
        LinuxBackendData* data = linux_data();
        Window owner = XGetSelectionOwner(data->display, data->clipboard_atom);
        if (owner == data->wind)
        {
            return ClipboardIdentity{ data->clipboard_sequence };
        }
        // It's always going to be the _next_ id so we can request it each time.
        return ClipboardIdentity{ ++data->clipboard_sequence };
    }

    // Queries.
    // System information.
    const SystemInfo* system_info()
    {
        LinuxBackendData* data = linux_data();
        return &data->sys_info;
    }

    // Time (in ms).
    Ticks get_ticks()
    {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now);
        LinuxBackendData* data = linux_data();
        uint64_t s = data->start_time.tv_sec * Thousand(1) + data->start_time.tv_nsec / Million(1);
        uint64_t n = now.tv_sec * Thousand(1) + now.tv_nsec / Million(1);
        return static_cast<Ticks>(n - s);
    }

    Ticks32 get_ticks32()
    {
        return static_cast<Ticks32>(get_ticks());
    }

    MicroSec now_microseconds()
    {
        timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        uint64_t result = t.tv_sec*Million(1) + (t.tv_nsec/Thousand(1));
        return MicroSec{ result };
    }

    // Monitor.
    Hz monitor_refresh_rate()
    {
        return linux_data()->refresh_rate;
    }

    Hz recompute_monitor_refresh_rate()
    {
        LinuxBackendData* data = linux_data();

        // Borrowed from: https://stackoverflow.com/a/17798269.
        XRRScreenConfiguration* conf = XRRGetScreenInfo(data->display, data->wind);
        short current_rate = XRRConfigCurrentRate(conf);
        // Give some sane default.
        if (current_rate < 30)
            return Hz{ 30 };

        return Hz(current_rate);
    }

    ScreenDimensions window_size()
    {
        return linux_data()->screen_size;
    }

    ScreenDimensions client_size()
    {
        LinuxBackendData* data = linux_data();
        XWindowAttributes attrs = {};
        XGetWindowAttributes(data->display, data->wind, &attrs);
        return { .width = Width{ attrs.width }, .height = Height{ attrs.height } };
    }

    DPI monitor_dpi()
    {
        // NYI.
        return DPI{ 96 };
    }

    // Paths / File IO.
    Error exe_path(Arena::Arena* arena, String8* buf)
    {
        *buf = read_symlink(arena, str8_mut(str8_literal("/proc/self/exe")));
        // Now we will chop the exe name.
        auto last_slash = str8_find_last_of(*buf, str8_mut(str8_literal("/")));
        if (last_slash != str8_index_sentinel)
        {
            *buf = str8_substr(*buf, { .off = 0, .len = last_slash });
        }
        return Error::None;
    }

    Error app_path(Arena::Arena* arena, String8* buf, String8 org, String8 app)
    {
        auto scratch = Arena::scratch_begin({ &arena, 1 });
        String8List lst{};
        String8 home = str8_cstr(getenv("HOME"));
        String8 prefix = str8_mut(str8_literal("/."));
        str8_list_push(scratch.arena, &lst, home);
        str8_list_push(scratch.arena, &lst, prefix);
        str8_list_push(scratch.arena, &lst, org);
        // Make a temp string for this.
        String8 joined = str8_list_join(scratch.arena, lst);
        if (mkdir(joined.str, 0755) == -1)
        {
            if (errno != EEXIST)
            {
                Arena::scratch_end(scratch);
                return Error::CreateDirectoryFailed;
            }
        }
        lst = String8List{};
        String8 sep = str8_mut(str8_literal("/"));
        str8_list_push(scratch.arena, &lst, joined);
        str8_list_push(scratch.arena, &lst, sep);
        str8_list_push(scratch.arena, &lst, app);
        joined = str8_list_join(scratch.arena, lst);
        if (mkdir(joined.str, 0755) == -1)
        {
            if (errno != EEXIST)
            {
                Arena::scratch_end(scratch);
                return Error::CreateDirectoryFailed;
            }
        }
        // Now that the directory has been created, we're going to assemble the final
        // path name by appending the '/' to the end and placing it in the final arena.
        *buf = str8_cat(arena, joined, sep);
        Arena::scratch_end(scratch);
        return Error::None;
    }

    FileHandle open_file(String8 file, FileAccess access)
    {
        int lnx_flags = 0;
        // Implies exactly.
        if(implies(access, FileAccess::Read | FileAccess::Write))
        {
            lnx_flags = O_RDWR;
            lnx_flags |= O_CREAT;
        }
        else if(implies(access, FileAccess::Write))
        {
            lnx_flags = O_WRONLY;
            lnx_flags |= O_CREAT;
            lnx_flags |= O_TRUNC;
        }
        else if(implies(access, FileAccess::Read))
        {
            lnx_flags = O_RDONLY;
        }

        if(implies(access, FileAccess::Append))
        {
            lnx_flags |= O_APPEND;
            lnx_flags |= O_CREAT;
        }

        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Need null-termination.
        file = str8_copy(scratch.arena, file);
        int fd = open(file.str, lnx_flags, 0655);
        Arena::scratch_end(scratch);
        if (fd == -1)
            return FileHandle::Sentinel;
        return os_file_handle(fd);
    }

    void close_file(FileHandle handle)
    {
        assert(handle != FileHandle::Sentinel);
        close(linux_file_handle(handle));
    }

    FileLength file_length(FileHandle handle)
    {
        struct stat file_stat{};
        int result = fstat(linux_file_handle(handle), &file_stat);
        if (result == -1)
            return FileLength::Sentinel;
        return FileLength(file_stat.st_size);
    }

    FileLength read_file(Arena::Arena* arena, String8* buf, FileHandle handle, FileLength count)
    {
        int fd = linux_file_handle(handle);
        uint64_t total_num_bytes_to_read = rep(count);
        uint64_t total_num_bytes_read = 0;
        uint64_t total_num_bytes_left_to_read = total_num_bytes_to_read;
        *buf = str8_cstr_alloc(arena, rep(count));
        for(;total_num_bytes_left_to_read > 0;)
        {
            int read_result = pread(fd, buf->str + total_num_bytes_read, total_num_bytes_left_to_read, total_num_bytes_read);
            if(read_result >= 0)
            {
                total_num_bytes_read += read_result;
                total_num_bytes_left_to_read -= read_result;
            }
            else if(errno != EINTR)
            {
                break;
            }
        }
        return FileLength{ total_num_bytes_read };
    }

    BytesWritten write_file(FileHandle handle, FileOffset off, String8 buf)
    {
        int fd = linux_file_handle(handle);
        uint64_t total_num_bytes_to_write = buf.size;
        uint64_t total_num_bytes_written = 0;
        uint64_t total_num_bytes_left_to_write = total_num_bytes_to_write;
        for(;total_num_bytes_left_to_write > 0;)
        {
            int write_result = pwrite(fd, buf.str + total_num_bytes_written, total_num_bytes_left_to_write, rep(off) + total_num_bytes_written);
            if(write_result >= 0)
            {
                total_num_bytes_written += write_result;
                total_num_bytes_left_to_write -= write_result;
            }
            else if(errno != EINTR)
            {
                break;
            }
        }
        return BytesWritten{ total_num_bytes_written };
    }

    FileProperties file_properties(String8 path)
    {
        struct stat file_stat{};
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Null-terminate.
        path = str8_copy(scratch.arena, path);
        int result = stat(path.str, &file_stat);
        Arena::scratch_end(scratch);
        FileProperties props{};
        if (result == -1)
            return props;
        props.size = FileLength(file_stat.st_size);
        props.created = dense_time_from_timespec(file_stat.st_ctim);
        props.modified = dense_time_from_timespec(file_stat.st_mtim);
        if (file_stat.st_mode & S_IFDIR)
        {
            props.props |= FileProperty::Directory;
        }
        return props;
    }

    Error create_directory(String8 dir)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        dir = str8_copy(scratch.arena, dir);
        int make_dir_r = mkdir(dir.str, 0755);
        Arena::scratch_end(scratch);
        if (make_dir_r == -1)
        {
            if (errno != EEXIST)
                return Error::CreateDirectoryFailed;
        }
        return Error::None;
    }

    bool directory_exists(String8 dir)
    {
        return implies(file_properties(dir).props, FileProperty::Directory);
    }

    bool file_or_path_exists(String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Null-terminate.
        path = str8_copy(scratch.arena, path);
        auto result = access(path.str, F_OK);
        Arena::scratch_end(scratch);
        return result == 0;
    }

    bool regular_file_exists(String8 file)
    {
        struct stat file_stat{ };
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Null-terminate.
        file = str8_copy(scratch.arena, file);
        int result = stat(file.str, &file_stat);
        Arena::scratch_end(scratch);
        if (result == -1)
            return false;
        return (file_stat.st_mode & S_IFREG);
    }

    Error canonical_file_path(Arena::Arena* arena, String8* buf, String8 path)
    {
        char char_buf[internal_path_max];
        if (realpath(path.str, char_buf) == nullptr)
            return Error::CanonicalizeFailedToGetPathname;
        *buf = str8_copy(arena, str8_cstr(char_buf));
        return Error::None;
    }

    bool working_directory(Arena::Arena* arena, String8* buf)
    {
        char cwd_buf[internal_path_max];
        if (getcwd(cwd_buf, std::size(cwd_buf)) == nullptr)
            return false;
        *buf = str8_copy(arena, str8_cstr(cwd_buf));
        return true;
    }

    bool set_working_directory(String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Null-terminate.
        path = str8_copy(scratch.arena, path);
        int result = chdir(path.str);
        Arena::scratch_end(scratch);
        return result == 0;
    }

    // Directory iteration.
    DirIter open_dir_iter(String8 dir, DirIterFlags flags)
    {
        LinuxBackendData* data = linux_data();
        // Allocate new dir iter.
        DirIterData* dir_data = alloc_dir_iter_data(&data->dir_iter_data_alloc);
        // Open the directory.
        // We must null-terminate the string here.
        dir_data->core_dir = sv_str8(dir);
        dir_data->dir = opendir(dir_data->core_dir.c_str());
        if (dir_data->dir == nullptr)
        {
            release_dir_iter_data(&data->dir_iter_data_alloc, dir_data);
            return DirIter::Sentinel;
        }
        dir_data->flags = flags;
        return os_dir_iter(dir_data);
    }

    bool dir_iter_next(Arena::Arena* arena, DirIterResult* result, DirIter iter)
    {
        assert(iter != DirIter::Sentinel);
        DirIterData* dir_data = linux_dir_iter(iter);
        if (dir_data->dir == nullptr)
            return false;
        if (implies(dir_data->flags, DirIterFlags::Done))
            return false;
        bool found = false;
        do
        {
            bool usable = false;
            // Next entry.
            dir_data->dp = readdir(dir_data->dir);
            found = dir_data->dp != nullptr;

            String8 name = str8_empty;

            FileProperty props{};
            if (found)
            {
                usable = true;
                auto scratch = Arena::scratch_begin({ &arena, 1 });
                name = str8_cstr(dir_data->dp->d_name);
                // Populate the full filename first so we can stat it, then we'll
                // truncate it later if only the file name is requested.
                String8 dir_name = str8_cstr(dir_data->dp->d_name);
                String8 path = combine_paths(scratch.arena, str8_cppview(dir_data->core_dir), dir_name);
                result->path = path;
                result->props = file_properties(result->path);
                props = result->props.props;
                if (not implies(dir_data->flags, DirIterFlags::FullPath))
                {
                    result->path = dir_name;
                }
                // Now we can copy the resulting path into our higher lifetime.
                result->path = str8_copy(arena, result->path);
                Arena::scratch_end(scratch);
            }

            // Figure out if this is filtered.
            // Exclude meta directories.
            if (str8_match_exact(name, str8_mut(str8_literal(".")))
                or str8_match_exact(name, str8_mut(str8_literal(".."))))
            {
                usable = false;
            }

            if (implies(props, FileProperty::Directory))
            {
                usable = usable and not implies(dir_data->flags, DirIterFlags::SkipDirs);
            }
            else
            {
                usable = usable and not implies(dir_data->flags, DirIterFlags::SkipFiles);
            }

            // Populate result and finish.
            if (usable)
                break;

            // Break if we failed to find.
            if (not found)
            {
                dir_data->flags |= DirIterFlags::Done;
                break;
            }
        } while(true);
        return found;
    }

    void close_dir_iter(DirIter iter)
    {
        assert(iter != DirIter::Sentinel);
        LinuxBackendData* data = linux_data();
        DirIterData* dir_data = linux_dir_iter(iter);
        release_dir_iter_data(&data->dir_iter_data_alloc, dir_data);
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
        const char* last = relative_path.str + relative_path.size;
        const char* first = relative_path.str;
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
        // First separate the two paths by the '/' then combine them together.
        auto scratch = Arena::scratch_begin({ &arena, 1 });
        String8List split{};
        String8 path_seps = str8_mut(str8_literal("/"));
        SplitStringsInput split_in{
            .in = a,
            .seps = path_seps,
            .flags = SplitStringsFlags::NoResultClear // We want to append results.
        };
        split_strings(scratch.arena, &split, split_in);
        split_in.in = b;
        split_strings(scratch.arena, &split, split_in);
        // Combine them all together using our preferred system separator.
        // The start separator should be based on whether or not 'a' is a root path.
        String8 start_sep = str8_empty;
        if (a.size != 0 and path_separator(a.str[0]))
        {
            start_sep = str8_mut(str8_literal("/"));
        }
        JoinStringsInput join_in{
            .strings = split,
            .start_sep = start_sep,
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
    void open_url_in_browser(String8)
    {
        // NYI.
    }

    void open_path_in_explorer(String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String8List cmd_line{};
        String8 cmd = str8_cat(scratch.arena, str8_mut(str8_literal("xdg-open ")), path);
        str8_list_push(scratch.arena, &cmd_line, cmd);
        OS::ProcessLaunchParams process_in{
            .wd = str8_empty,
            .cmd_line = cmd_line,
            .env = {},
            .flags = OS::LaunchFlags::InheritEnv | OS::LaunchFlags::Consoleless
        };
        auto process = launch_process(process_in);
        detach_process(process);
        Arena::scratch_end(scratch);
    }

    void open_path_in_preferred_explorer(String8 exe, String8 path)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        String8List cmd_line{};
        String8 cmd = str8_fmt(scratch.arena, "%S %S", exe, path);
        str8_list_push(scratch.arena, &cmd_line, cmd);
        OS::ProcessLaunchParams process_in{
            .wd = str8_empty,
            .cmd_line = cmd_line,
            .env = {},
            .flags = OS::LaunchFlags::InheritEnv | OS::LaunchFlags::Consoleless
        };
        auto process = launch_process(process_in);
        detach_process(process);
        Arena::scratch_end(scratch);
    }

    // Gap-specific events.
    void post_thread_wakeup()
    {
        LinuxBackendData* data = linux_data();
        XEvent event{ };
        event.xany.type = ClientMessage;
        event.xclient.send_event = true;
        event.xclient.display = data->display;
        event.xclient.message_type = data->thread_wakeup_atom;
        event.xclient.format = 8;
        event.xclient.window = data->wind;
        event.xclient.data.l[0] = static_cast<long>(data->thread_wakeup_atom);

        XSendEvent(data->display, data->wind, False, NoEventMask, &event);
        XFlush(data->display);
    }

    // Library handling.
    LibraryHandle load_library(String8 lib_name)
    {
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Null-terminate.
        lib_name = str8_copy(scratch.arena, lib_name);
        void* so = dlopen(lib_name.str, RTLD_LAZY | RTLD_LOCAL);
        Arena::scratch_end(scratch);
        if (so == nullptr)
            return LibraryHandle::Sentinel;
        return os_library_handle(so);
    }

    void unload_library(LibraryHandle lib)
    {
        void* so = linux_library_handle(lib);
        dlclose(so);
    }

    void* get_function(LibraryHandle lib, String8 fn)
    {
        void* so = linux_library_handle(lib);
        void* proc = dlsym(so, fn.str);
        return proc;
    }

    LibraryHandle get_gl_library()
    {
        // Just return something close to a sentinel.  The system won't close it.
        return LibraryHandle{};
    }

    void* get_gl_function(LibraryHandle, String8 fn)
    {
        void* result = nullptr;
        auto scratch = Arena::scratch_begin(Arena::no_conflicts);
        // Null-terminate.
        fn = str8_copy(scratch.arena, fn);
        result = reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const unsigned char*>(fn.str)));
        if (result == nullptr)
        {
            result = reinterpret_cast<void*>(glXGetProcAddressARB(reinterpret_cast<const unsigned char*>(fn.str)));
        }
        Arena::scratch_end(scratch);
        return result;
    }

    // Process creation.
    ProcessHandle launch_process(const ProcessLaunchParams& in)
    {
        // TODO: This is incomplete.  We need to handle various flags from the input such as inheriting the environment (or not) and injecting new
        // environment variables.
        // Start the show!
        pid_t pid = fork();

        // fork failed.  Close the pipes.
        if (pid == -1)
            return ProcessHandle::Sentinel;

        // Child.
        if (pid == 0)
        {
            // Create a vector of the strings that can be
            // presented as argv.  So first we need to ensure
            // that every single string is null-terminated
            // and separated into the list.

            // We can allocate a new arena for this so we don't
            // run into the possibility to sharing the same
            // kernel object as the mmap memory from the parent
            // process arena.
            Arena::Arena* arena = Arena::alloc(Arena::default_params);
            String8List string_pool{};
            for EachNode(n, in.cmd_line.first)
            {
                String8 str = str8_copy(arena, n->string);
                str8_list_push(arena, &string_pool, str);
            }

            String8 wd = str8_copy(arena, in.wd);

            // Change to the working directory.
            set_working_directory(wd);

            // Convert to the argv array.
            // We are prepending "/bin/sh" and "-c", so we will
            // need to add +2 and +1 for the null string.
            uint64_t argc = 2 + string_pool.node_count + 1;
            char** argv = Arena::push_array<char*>(arena, argc);
            uint64_t i = 0;
            char sh[] = "/bin/sh";
            char dash_c[] = "-c";
            argv[i++] = sh;
            argv[i++] = dash_c;
            for EachNode(n, string_pool.first)
            {
                argv[i++] = n->string.str;
            }
            argv[i++] = nullptr;
            assert(i == argc);
            // Start execution.
            execvp("/bin/sh", argv);
            // Note: This is only reached if execl fails.
            exit(EXIT_FAILURE);
        }
        // Parent process.
        return os_process_handle(pid);
    }

    IOPipe launch_piped_process(const ProcessLaunchParams& in)
    {
        IOPipe ret = IOPipe::Sentinel;
        // Input pipes.
        PipePair in_pipes = create_pipes();
        if (not valid_pipe(in_pipes))
            return ret;

        // Output pipes.
        PipePair out_pipes = create_pipes();
        if (not valid_pipe(out_pipes))
        {
            close_pipes(in_pipes);
            return ret;
        }

        // Error pipes.
        PipePair err_pipes = create_pipes();
        if (not valid_pipe(err_pipes))
        {
            close_pipes(in_pipes);
            close_pipes(out_pipes);
            return ret;
        }

        // Setup IO in non-blocking mode.
        // stdout.
        int flags = fcntl(out_pipes.pipes[0], F_GETFL, 0);
        fcntl(out_pipes.pipes[0], F_SETFL, flags | O_NONBLOCK);
        // stderr.
        flags = fcntl(err_pipes.pipes[0], F_GETFL, 0);
        fcntl(err_pipes.pipes[0], F_SETFL, flags | O_NONBLOCK);

        // Start the show!
        pid_t pid = fork();

        // fork failed.  Close the pipes.
        if (pid == -1)
        {
            close_pipes(in_pipes);
            close_pipes(out_pipes);
            close_pipes(err_pipes);
            return ret;
        }

        // Child.
        if (pid == 0)
        {
            // Discard the write end of input.
            close(in_pipes.pipes[1]);
            // Discard the read ends.
            close(out_pipes.pipes[0]);
            close(err_pipes.pipes[0]);

            // Dup the reads.
            dup2(in_pipes.pipes[0], fileno(stdin));
            dup2(out_pipes.pipes[1], fileno(stdout));
            dup2(err_pipes.pipes[1], fileno(stderr));

            // Create a vector of the strings that can be
            // presented as argv.  So first we need to ensure
            // that every single string is null-terminated
            // and separated into the list.

            // We can allocate a new arena for this so we don't
            // run into the possibility to sharing the same
            // kernel object as the mmap memory from the parent
            // process arena.
            Arena::Arena* arena = Arena::alloc(Arena::default_params);
            String8List string_pool{};
            for EachNode(n, in.cmd_line.first)
            {
                String8 str = str8_copy(arena, n->string);
                str8_list_push(arena, &string_pool, str);
            }

            String8 wd = str8_copy(arena, in.wd);

            // Change to the working directory.
            set_working_directory(wd);

            // Convert to the argv array.
            // We are prepending "/bin/sh" and "-c", so we will
            // need to add +2 and +1 for the null string.
            uint64_t argc = 2 + string_pool.node_count + 1;
            char** argv = Arena::push_array<char*>(arena, argc);
            uint64_t i = 0;
            char sh[] = "/bin/sh";
            char dash_c[] = "-c";
            argv[i++] = sh;
            argv[i++] = dash_c;
            for EachNode(n, string_pool.first)
            {
                argv[i++] = n->string.str;
            }
            argv[i++] = nullptr;
            assert(i == argc);
            // Start execution.
            execvp("/bin/sh", argv);
            // Note: This is only reached if execl fails.
            exit(EXIT_FAILURE);
        }
        // Parent process.
        else
        {
            // Close the write.
            close(in_pipes.pipes[0]);
            // Close the associated write pipes.
            close(out_pipes.pipes[1]);
            close(err_pipes.pipes[1]);
        }
        AllocPipeDataInput pipe_in{
            .child_pid = pid,
            .stdout_pipe = out_pipes.pipes[0],
            .stderr_pipe = err_pipes.pipes[0]
        };
        IOPipeData* pipe_data = alloc_pipe_data(&linux_data()->pipe_data_alloc, pipe_in);
        return os_pipe_handle(pipe_data);
    }

    ReadIOPipeResult read_piped_process(IOPipe piped_process, ReadIOPipeInput in)
    {
        // Async read on both pipe ends.
        ReadIOPipeResult result{};
        IOPipeData* pipe_data = linux_pipe_handle(piped_process);
        if (pipe_data->stdout_ep.read_pipe == -1
            and pipe_data->stderr_ep.read_pipe == -1)
        {
            // Wait for the child process to complete, but async.
            int status;
            int ret = waitpid(pipe_data->child_pid, &status, WNOHANG);
            bool release_data = false;
            if (ret == -1)
            {
                result.exit_code = 1;
                result.pending_output = false;
                release_data = true;
            }
            else if (ret == 0)
            {
                // Wait for the process to fully exit.
                result.pending_output = true;
            }
            else if (ret == pipe_data->child_pid)
            {
                result.pending_output = false;
                // Get the exit code.
                if (WIFEXITED(status))
                {
                    result.exit_code = WEXITSTATUS(status);
                }
                else
                {
                    // Others?  See man 2 waitpid.
                    result.exit_code = 1;
                }
                release_data = true;
            }
            else
            {
                // Catchall.
                result.exit_code = -1;
                result.pending_output = false;
                release_data = true;
            }

            if (release_data)
            {
                release_pipe_data(&linux_data()->pipe_data_alloc, pipe_data);
            }
            return result;
        }

        // Assume more pending data until both pipes are closed and we enter the case above.
        result.pending_output = true;

        if (pipe_data->stdout_ep.read_pipe != -1)
        {
            result.std_out_write = read_pipe_endpoint(in.arena, in.std_out, &pipe_data->stdout_ep);
        }

        if (pipe_data->stderr_ep.read_pipe != -1)
        {
            result.std_err_write = read_pipe_endpoint(in.arena, in.std_err, &pipe_data->stderr_ep);
        }
        return result;
    }

    void join_process(ProcessHandle)
    {
        // NYI.
    }

    void detach_process(ProcessHandle)
    {
        // Nothing to do in Linux.
    }

    void terminate_process(ProcessHandle process)
    {
        pid_t pid = linux_process_handle(process);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }

    void close_pipes_and_terminate_process(IOPipe piped_process)
    {
        IOPipeData* pipe_data = linux_pipe_handle(piped_process);
        // Kill the process first.
        kill(pipe_data->child_pid, SIGKILL);
        waitpid(pipe_data->child_pid, nullptr, 0);
        // Close the pipes.
        close(pipe_data->stdout_ep.read_pipe);
        close(pipe_data->stderr_ep.read_pipe);
        // Finally, invalidate the object.
        release_pipe_data(&linux_data()->pipe_data_alloc, pipe_data);
    }

    // Memory allocation.
    void* mem_reserve(AllocationSize size)
    {
        void* result = mmap(0, rep(size), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (result == MAP_FAILED)
        {
            result = nullptr;
        }
        return result;
    }

    bool mem_commit(void* ptr, AllocationSize size)
    {
        int result = mprotect(ptr, rep(size), PROT_READ | PROT_WRITE);
        return result == 0;
    }

    void mem_decommit(void* ptr, AllocationSize size)
    {
        madvise(ptr, rep(size), MADV_DONTNEED);
        mprotect(ptr, rep(size), PROT_NONE);
    }

    void mem_release(void* ptr, AllocationSize size)
    {
        munmap(ptr, rep(size));
    }

    void* mem_reserve_large(AllocationSize size)
    {
        void* result = mmap(0, rep(size), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (result == MAP_FAILED)
        {
            result = nullptr;
        }
        return result;
    }

    bool mem_commit_large(void* ptr, AllocationSize size)
    {
        int result = mprotect(ptr, rep(size), PROT_READ | PROT_WRITE);
        return result == 0;
    }

    void mem_clear_working_set_pages()
    {
        // NYI.
    }

    // Threading.
    Thread launch_thread(ThreadEntryPointFunctionType entry, void* data_p)
    {
        LinuxBackendData* data = linux_data();
        LinuxEntity* thread = linux_alloc_thread(data, entry, data_p);
        return os_thread(thread);
    }

    bool join_thread(Thread thread)
    {
        LinuxBackendData* data = linux_data();
        LinuxEntity* e = linux_thread(thread);
        int join_result = pthread_join(e->thread.handle, nullptr);
        linux_release_thread(data, thread);
        return join_result == 0;
    }

    ThreadID thread_id()
    {
        return ThreadID{ static_cast<uint32_t>(gettid()) };
    }

    // Synchronization primitives.
    // Mutex.
    Mutex alloc_mutex()
    {
        LinuxBackendData* data = linux_data();
        return os_mutex(linux_mutex_alloc(data));
    }

    void release_mutex(Mutex mutex)
    {
        LinuxBackendData* data = linux_data();
        release_linux_mutex(data, mutex);
    }

    void lock_mutex(Mutex mutex)
    {
        LinuxEntity* e = linux_mutex(mutex);
        pthread_mutex_lock(&e->mutex);
    }

    void unlock_mutex(Mutex mutex)
    {
        LinuxEntity* e = linux_mutex(mutex);
        pthread_mutex_unlock(&e->mutex);
    }

    // Condition variable.
    ConditionVariable alloc_condition_var()
    {
        LinuxBackendData* data = linux_data();
        LinuxEntity* cv = linux_condition_var_alloc(data);
        if (cv == nullptr)
            return ConditionVariable::Sentinel;
        return os_condition_var(cv);
    }

    void release_condition_var(ConditionVariable cv)
    {
        LinuxBackendData* data = linux_data();
        release_linux_condition_var(data, cv);
    }

    bool wait_condition_var(ConditionVariable cv, Mutex mutex, MicroSec end_us)
    {
        LinuxEntity* l_cv = linux_condition_var(cv);
        LinuxEntity* l_mutex = linux_mutex(mutex);
        int result;
        if (end_us == MicroSec::Infinite)
        {
            result = pthread_cond_wait(&l_cv->cv, &l_mutex->mutex);
        }
        else
        {
            timespec end_timespec{};
            end_timespec.tv_sec = rep(end_us)/Million(1);
            // Chop the seconds out.
            end_timespec.tv_nsec = Thousand(1) * (rep(end_us) - (rep(end_us) / Million(1)) * Million(1));
            result = pthread_cond_timedwait(&l_cv->cv, &l_mutex->mutex, &end_timespec);
        }
        return result != ETIMEDOUT;
    }

    void notify_one_condition_var(ConditionVariable cv)
    {
        LinuxEntity* l_cv = linux_condition_var(cv);
        pthread_cond_signal(&l_cv->cv);
    }

    void notify_all_condition_var(ConditionVariable cv)
    {
        LinuxEntity* l_cv = linux_condition_var(cv);
        pthread_cond_broadcast(&l_cv->cv);
    }

    // Setup for core rendering.
    void populate_core_render_data(RenderCoreData* render_data)
    {
        LinuxBackendData* data = linux_data();
        data->render_data = render_data;
    }
} // namespace OS

int main(int argc, char** argv)
{
    // Setup the system info.
    OS::LinuxBackendData* data = OS::linux_data();
    {
        data->sys_info.processor_count = OS::ProcCount(get_nprocs());
        data->sys_info.page_size = OS::PageSize(getpagesize());
        data->sys_info.large_page_size = OS::PageSize{ MB(2) };
        data->sys_info.allocation_granularity = OS::AllocGranularity{ rep(data->sys_info.page_size) };
    }

#ifdef BUILD_TRACK_ARENA
    Arena::init_tracker_arena();
#endif // BUILD_TRACK_ARENA

    // Setup thread context.
    {
        Thread::TLD* tl_ctx = Thread::tld_alloc();
        Thread::tld_select(tl_ctx);
    }

    // Setup dynamic alloc state.
    {
        data->linux_arena = Arena::alloc(Arena::default_params);
    }

    // Computer name can be allocated here.
    {
        // man(2) gethostname:
        // > POSIX.1 guarantees that "Host names (not including the terminating null byte) are limited to HOST_NAME_MAX bytes".
        // Which implies we need to add an extra byte.
        constexpr int buf_size = HOST_NAME_MAX + 1;
        char buf[buf_size];
        int result = gethostname(buf, buf_size);
        if (result == 0)
        {
            String8 str = str8_cstr(buf);
            // Copy into our arena.
            data->sys_info.machine_name = str8_copy(data->linux_arena, str);
        }
        // Note: if somehow the hostname violates the buffer size above, no host name for you, for now...
    }

    // Finally, we can set up the entity mutex.
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        // From man:
        // > pthread_mutex_init always returns 0.
        pthread_mutex_init(&data->linux_arena_mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }

#ifdef BUILD_TRACK_ARENA
    Arena::init_tracker_mutex();
#endif // BUILD_TRACK_ARENA

    // Initialize other Linux data.
    init_linux(data);

    return gap_main_entry(argc, argv);
}
