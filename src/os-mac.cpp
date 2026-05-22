#include "os.h"

#include <cassert>

#include <forward_list>
#include <string_view>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <copyfile.h>
#include <time.h>
#include <unistd.h>
#include <libproc.h>

#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>

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

@interface OSMacWindow : NSWindow<NSWindowDelegate>
{
}
@property (nonatomic) NSEventModifierFlags previous_flags;
@end

namespace OS
{
  namespace
  {
    using namespace Constants;
    constexpr int internal_max_path = 1024 * 2;
    constexpr uint64_t pipe_buffer_size = KB(16);

    struct MacEntity;


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
      DIR *dir;
      dirent *dp;
      std::string core_dir;
      DirIterFlags flags;
    };

    using DirIterDataAlloc = std::forward_list<DirIterData>;

    struct ThreadData
    {
      ThreadEntryPointFunctionType func;
      void *data_p;
      pthread_t handle;
    };

    struct MacEntity
    {
      MacEntity *next;
      MacEntity *prev;
      union
      {
        //- brt: thread
        ThreadData thread;

        //- brt: mutex
        pthread_mutex_t mutex;

        //- brt: condition variable
        pthread_cond_t cv;
      };
      uint64_t gen;
    };

    struct MacEntityList
    {
      MacEntity *first;
      MacEntity *last;
      uint64_t count;
    };

    DirIterData *alloc_dir_iter_data(DirIterDataAlloc *alloc)
    {
      alloc->push_front({});
      DirIterData *data = &alloc->front();
      data->dir = nullptr;
      data->dp = nullptr;
      return data;
    }

    void release_dir_iter_data(DirIterDataAlloc *alloc, DirIterData *data)
    {
      if (data->dir != nullptr)
      {
        closedir(data->dir);
      }
      ListHelpers::remove_list_element(alloc, data);
    }

    struct MacBackendData
    {
      RenderCoreData *render_data;
      pthread_mutex_t mac_arena_mutex;
      Arena::Arena *mac_arena;
      MacEntityList thread_lst;
      MacEntityList mutex_lst;
      MacEntityList cond_var_lst;
      MacEntity *entity_free_list;
      SystemInfo sys_info;
      uint64_t start_time_ns;
      DirIterDataAlloc dir_iter_data_alloc;
      Ticks double_click_time;
      bool fullscreened;
      Arena::Arena *events_arena;
      Events *events;
      OSMacWindow *wind;
    };

    MacBackendData impl_data;

    MacBackendData *mac_data()
    {
      return &impl_data;
    }
    
    MacEntity *push_entity(MacBackendData *data, MacEntityList *lst)
    {
      MacEntity *node = nullptr;
      pthread_mutex_lock(&data->mac_arena_mutex);
      if (data->entity_free_list != nullptr)
      {
        node = data->entity_free_list;
        SLLStackPop(data->entity_free_list);
        zero_bytes(node);
      }
      else
      {
        node = Arena::push_array<MacEntity>(data->mac_arena, 1);
      }
      DLLPushBack(lst->first, lst->last, node);
      ++lst->count;
      pthread_mutex_unlock(&data->mac_arena_mutex);
      return node;
    }

    void release_entity(MacBackendData *data, MacEntityList *lst, MacEntity *e)
    {
      pthread_mutex_lock(&data->mac_arena_mutex);
      DLLRemove(lst->first, lst->last, e);
      --lst->count;
      SLLStackPush(data->entity_free_list, e);
      pthread_mutex_unlock(&data->mac_arena_mutex);
    }
    
    OSWindow os_window(OSMacWindow *wnd)
    {
      return OSWindow((uintptr_t)wnd);
    }

    OSMacWindow *mac_window(OSWindow wnd)
    {
      return (OSMacWindow *)(rep(wnd));
    }

    FileHandle os_file_handle(int fd)
    {
      return FileHandle(fd);
    }

    int mac_file_handle(FileHandle fd)
    {
      return static_cast<int>(rep(fd));
    }

    LibraryHandle os_library_handle(void *so)
    {
      return LibraryHandle{ reinterpret_cast<size_t>(so) };
    }

    void *mac_library_handle(LibraryHandle handle)
    {
      return reinterpret_cast<void *>(rep(handle));
    }

    IOPipe os_pipe_handle(IOPipeData *p)
    {
      return IOPipe{ reinterpret_cast<PrimitiveType<IOPipe>>(p) };
    }

    DirIter os_dir_iter(DirIterData *d)
    {
      return DirIter{ reinterpret_cast<PrimitiveType<DirIter>>(d) };
    }

    DirIterData *mac_dir_iter(DirIter h)
    {
      return reinterpret_cast<DirIterData *>(h);
    }

    ProcessHandle os_process_handle(pid_t id)
    {
      return static_cast<ProcessHandle>(id);
    }

    pid_t mac_process_handle(ProcessHandle h)
    {
      return static_cast<pid_t>(h);
    }

    NSString *ns_string_from_str8(String8 string)
    {
      NSString *result = [[NSString alloc] initWithBytes:string.str
                                                  length:string.size
                                                encoding:NSUTF8StringEncoding];
      return result;
    }


    uint64_t sysctl_u64(char *name)
    {
      uint64_t result = 0;

      uint8_t buff[8];
      size_t buff_len = sizeof(buff);

      if (sysctlbyname(name, buff, &buff_len, 0, 0) != -1)
      {
        memcpy(&result, buff, buff_len);
      }

      return result;
    }

    bool init_mac(MacBackendData *data)
    {
      data->start_time_ns = clock_gettime_nsec_np(CLOCK_REALTIME);
      uint32_t double_click_time_ms = (uint32_t)([NSEvent doubleClickInterval] * 1000.0);
      data->double_click_time = Ticks{ double_click_time_ms };

      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
      [NSApp finishLaunching]; 

      return true;
    }

    KeyMods os_mac_get_modifiers()
    {
      KeyMods modifiers = KeyMods::None;
      CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
      if (flags & kCGEventFlagMaskControl)
      {
        modifiers |= KeyMods::Ctrl;
      }
      if (flags & kCGEventFlagMaskShift)
      {
        modifiers |= KeyMods::Shift;
      }
      if (flags & kCGEventFlagMaskAlternate)
      {
        modifiers |= KeyMods::Alt;
      }
      if (flags & kCGEventFlagMaskCommand)
      {
        modifiers |= KeyMods::Cmd;
      }
      return modifiers;
    }

    Event *push_event(EventSort sort, OSWindow window)
    {
      MacBackendData *data = mac_data();
      Event *e = Arena::push_array<Event>(data->events_arena, 1);
      e->sort = sort;
      e->window = window;
      e->modifiers = os_mac_get_modifiers();
      SLLQueuePush(data->events->first, data->events->last, e);
      ++data->events->count;
      return e;
    }

    Key os_mac_os_key_from_vkey( uint32_t vkey )
    {
      static bool first = true;
      static Key key_table[512] = {};
      if (first)
      {
        first = false;
        key_table[kVK_ANSI_A] = Key::A;
        key_table[kVK_ANSI_B] = Key::B;
        key_table[kVK_ANSI_C] = Key::C;
        key_table[kVK_ANSI_D] = Key::D;
        key_table[kVK_ANSI_E] = Key::E;
        key_table[kVK_ANSI_F] = Key::F;
        key_table[kVK_ANSI_G] = Key::G;
        key_table[kVK_ANSI_H] = Key::H;
        key_table[kVK_ANSI_I] = Key::I;
        key_table[kVK_ANSI_J] = Key::J;
        key_table[kVK_ANSI_K] = Key::K;
        key_table[kVK_ANSI_L] = Key::L;
        key_table[kVK_ANSI_M] = Key::M;
        key_table[kVK_ANSI_N] = Key::N;
        key_table[kVK_ANSI_O] = Key::O;
        key_table[kVK_ANSI_P] = Key::P;
        key_table[kVK_ANSI_Q] = Key::Q;
        key_table[kVK_ANSI_R] = Key::R;
        key_table[kVK_ANSI_S] = Key::S;
        key_table[kVK_ANSI_T] = Key::T;
        key_table[kVK_ANSI_U] = Key::U;
        key_table[kVK_ANSI_V] = Key::V;
        key_table[kVK_ANSI_W] = Key::W;
        key_table[kVK_ANSI_X] = Key::X;
        key_table[kVK_ANSI_Y] = Key::Y;
        key_table[kVK_ANSI_Z] = Key::Z;
        key_table[kVK_ANSI_0] = Key::_0;
        key_table[kVK_ANSI_1] = Key::_1;
        key_table[kVK_ANSI_2] = Key::_2;
        key_table[kVK_ANSI_3] = Key::_3;
        key_table[kVK_ANSI_4] = Key::_4;
        key_table[kVK_ANSI_5] = Key::_5;
        key_table[kVK_ANSI_6] = Key::_6;
        key_table[kVK_ANSI_7] = Key::_7;
        key_table[kVK_ANSI_8] = Key::_8;
        key_table[kVK_ANSI_9] = Key::_9;
        key_table[kVK_ANSI_Keypad0] = Key::Num0;
        key_table[kVK_ANSI_Keypad1] = Key::Num1;
        key_table[kVK_ANSI_Keypad2] = Key::Num2;
        key_table[kVK_ANSI_Keypad3] = Key::Num3;
        key_table[kVK_ANSI_Keypad4] = Key::Num4;
        key_table[kVK_ANSI_Keypad5] = Key::Num5;
        key_table[kVK_ANSI_Keypad6] = Key::Num6;
        key_table[kVK_ANSI_Keypad7] = Key::Num7;
        key_table[kVK_ANSI_Keypad8] = Key::Num8;
        key_table[kVK_ANSI_Keypad9] = Key::Num9;
        key_table[kVK_F1] = Key::F1;
        key_table[kVK_F2] = Key::F2;
        key_table[kVK_F3] = Key::F3;
        key_table[kVK_F4] = Key::F4;
        key_table[kVK_F5] = Key::F5;
        key_table[kVK_F6] = Key::F6;
        key_table[kVK_F7] = Key::F7;
        key_table[kVK_F8] = Key::F8;
        key_table[kVK_F9] = Key::F9;
        key_table[kVK_F10] = Key::F10;
        key_table[kVK_F11] = Key::F11;
        key_table[kVK_F12] = Key::F12;
        key_table[kVK_F13] = Key::F13;
        key_table[kVK_F14] = Key::F14;
        key_table[kVK_F15] = Key::F15;
        key_table[kVK_F16] = Key::F16;
        key_table[kVK_F17] = Key::F17;
        key_table[kVK_F18] = Key::F18;
        key_table[kVK_F19] = Key::F19;
        key_table[kVK_F20] = Key::F20;

        // --- Punctuation and symbols ---
        key_table[kVK_Space]           = Key::Space;
        key_table[kVK_ANSI_Grave]      = Key::Tick;          // ` (same as VK_OEM_3)
        key_table[kVK_ANSI_Minus]      = Key::Minus;         // -
        key_table[kVK_ANSI_Equal]      = Key::Equal;         // =
        key_table[kVK_ANSI_LeftBracket]= Key::LeftBracket;   // [
        key_table[kVK_ANSI_RightBracket]=Key::RightBracket;  // ]
        key_table[kVK_ANSI_Semicolon]  = Key::Semicolon;     // ;
        key_table[kVK_ANSI_Quote]      = Key::Quote;         // '
        key_table[kVK_ANSI_Comma]      = Key::Comma;         // ,
        key_table[kVK_ANSI_Period]     = Key::Period;        // .
        key_table[kVK_ANSI_Slash]      = Key::Slash;         // /
        key_table[kVK_ANSI_Backslash]  = Key::BackSlash;     // \

        // --- Control / function keys ---
        key_table[kVK_Tab]             = Key::Tab;
        key_table[kVK_Escape]          = Key::Esc;
        key_table[kVK_Return]          = Key::Return;
        key_table[kVK_Delete]          = Key::Backspace;     // Backspace (Delete on Mac)
        key_table[kVK_ForwardDelete]   = Key::Delete;        // Forward delete (fn+Delete)
        key_table[kVK_Help]            = Key::Insert;        // Insert = Help key on extended keyboard

        key_table[kVK_PageUp]          = Key::PageUp;
        key_table[kVK_PageDown]        = Key::PageDown;
        key_table[kVK_Home]            = Key::Home;
        key_table[kVK_End]             = Key::End;

        key_table[kVK_UpArrow]         = Key::Up;
        key_table[kVK_LeftArrow]       = Key::Left;
        key_table[kVK_DownArrow]       = Key::Down;
        key_table[kVK_RightArrow]      = Key::Right;

        // --- Locks and modifiers ---
        key_table[kVK_CapsLock]        = Key::CapsLock;
        key_table[kVK_Function]        = Key::Menu;          // “Fn” often used as Menu-equivalent
        key_table[kVK_Command]         = Key::Ctrl;          // ⌘ as Ctrl analog
        key_table[kVK_Control]         = Key::Ctrl;
        key_table[kVK_RightControl]    = Key::Ctrl;
        key_table[kVK_Shift]           = Key::Shift;
        key_table[kVK_RightShift]      = Key::Shift;
        key_table[kVK_Option]          = Key::Alt;
        key_table[kVK_RightOption]     = Key::Alt;

        // --- Numeric keypad ---
        key_table[kVK_ANSI_KeypadDivide]   = Key::NumSlash;
        key_table[kVK_ANSI_KeypadMultiply] = Key::NumStar;
        key_table[kVK_ANSI_KeypadMinus]    = Key::NumMinus;
        key_table[kVK_ANSI_KeypadPlus]     = Key::NumPlus;
        key_table[kVK_ANSI_KeypadDecimal]  = Key::NumPeriod;
      }
      return key_table[vkey];
    }


    Mutex os_mutex(MacEntity *e)
    {
      return Mutex{ reinterpret_cast<PrimitiveType<Mutex>>(e) };
    }

    MacEntity *mac_mutex(Mutex m)
    {
      return reinterpret_cast<MacEntity *>(m);
    }
    
    MacEntity *mac_mutex_alloc(MacBackendData *data)
    {
      MacEntity *mutex = push_entity(data, &data->mutex_lst);
      pthread_mutexattr_t attr;
      pthread_mutexattr_init(&attr);
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init(&mutex->mutex, &attr);
      pthread_mutexattr_destroy(&attr);
      return mutex;
    }
    
    void release_mac_mutex(MacBackendData *data, Mutex m)
    {
      MacEntity *e = mac_mutex(m);
      pthread_mutex_destroy(&e->mutex);
      release_entity(data, &data->mutex_lst, e);
    }

    ConditionVariable os_condition_var(MacEntity *e)
    {
      return ConditionVariable{ reinterpret_cast<PrimitiveType<ConditionVariable>>(e) };
    }
    
    MacEntity *mac_condition_var(ConditionVariable cv)
    {
      return reinterpret_cast<MacEntity *>(cv);
    }

    MacEntity *mac_condition_var_alloc(MacBackendData *data)
    {
      MacEntity *cv = push_entity(data, &data->cond_var_lst);
      int init_result = pthread_cond_init(&cv->cv, nullptr);
      if (init_result != 0)
      {
        release_entity(data, &data->cond_var_lst, cv);
        return nullptr;
      }
      return cv;
    }
    
    void release_mac_condition_var(MacBackendData *data, ConditionVariable cv)
    {
      MacEntity *e = mac_condition_var(cv);
      pthread_cond_destroy(&e->cv);
      release_entity(data, &data->cond_var_lst, mac_condition_var(cv));
    }

    Thread os_thread(MacEntity *e)
    {
      return Thread{ reinterpret_cast<PrimitiveType<Thread>>(e) };
    }

    MacEntity *mac_thread(Thread t)
    {
      return reinterpret_cast<MacEntity *>(t);
    }

    void *mac_thread_entry_point(void *ptr)
    {
      MacEntity *e = static_cast<MacEntity *>(ptr);
      thread_entry_bridge(e->thread.func, e->thread.data_p);
      return nullptr;
    }

    MacEntity *mac_alloc_thread(MacBackendData *data, ThreadEntryPointFunctionType func, void *data_p)
    {
      MacEntity *thread = push_entity(data, &data->thread_lst);
      thread->thread.func = func;
      thread->thread.data_p = data_p;
      int result = pthread_create(&thread->thread.handle, nullptr, mac_thread_entry_point, thread);
      if (result != 0)
      {
        release_entity(data, &data->thread_lst, thread);
        thread = nullptr;
      }
      return thread;
    }

    void mac_release_thread(MacBackendData *data, Thread thread)
    {
      release_entity(data, &data->thread_lst, mac_thread(thread));
    }
  } //namespace [anon]

  ///////////////////////////////////////////////////////////////////////////////
  //~ brt: START GAP API

#ifdef MAC_GFX

  OSWindow init_window(Vec4i wind_rect, String8 title)
  {
    MacBackendData *data = mac_data();
    OSWindow result = OSWindow::Sentinel;
    @autoreleasepool
    {
      wind_rect.x = wind_rect.x == default_window_pos.x ? 0 : wind_rect.x;
      wind_rect.y = wind_rect.y == default_window_pos.y ? 0 : wind_rect.y;
      float scale = 1.0/NSScreen.mainScreen.backingScaleFactor;
      NSRect rect = NSMakeRect(wind_rect.x*scale, wind_rect.y*scale, wind_rect.z*scale, wind_rect.a*scale);
      NSUInteger mask = NSWindowStyleMaskTitled |
        NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable |
        NSWindowStyleMaskResizable;

      //- brt: create window
      OSMacWindow *ns_window = [[OSMacWindow alloc] initWithContentRect:rect
                                                              styleMask:mask
                                                                backing:NSBackingStoreBuffered
                                                                  defer:NO];
      data->wind = ns_window;

      NSString *ns_title = ns_string_from_str8(title);
      [ns_window setTitle:ns_title];

      ns_window.delegate = ns_window;
      [ns_window registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
      [ns_window setAcceptsMouseMovedEvents:YES];

      [NSApp activateIgnoringOtherApps:YES];
      result = os_window(ns_window);

      //- brt: equip renderer
      if (!Render::os_init_renderer_window(result))
      {
        result = OSWindow::Sentinel;
      }

      [ns_window makeKeyAndOrderFront:nil];
    }
    return result;
  }

#endif

  void set_cursor(CursorStyle style)
  {
    NSCursor *ns_cursor = nil;
    switch(style)
    {
      case CursorStyle::IBeam: ns_cursor = [NSCursor IBeamCursor]; break;
      case CursorStyle::Select: ns_cursor = [NSCursor pointingHandCursor]; break;
      case CursorStyle::Default: ns_cursor = [NSCursor arrowCursor]; break;
      case CursorStyle::UpDownArrow: ns_cursor = [NSCursor resizeUpDownCursor]; break;
      case CursorStyle::LeftRightArrow: ns_cursor = [NSCursor resizeLeftRightCursor]; break;
      case CursorStyle::SouthEastArrow: ns_cursor = [NSCursor openHandCursor]; break;
      case CursorStyle::SouthWestArrow: ns_cursor = [NSCursor openHandCursor]; break;
      case CursorStyle::SizeAll: ns_cursor = [NSCursor openHandCursor]; break;
    }
    if (ns_cursor != nil)
    {
      [ns_cursor set];
    }
  }

  void *mem_reserve(AllocationSize size)
  {
    void *result = mmap(0, rep(size), PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if(result == MAP_FAILED)
    {
      result = nullptr;
    }
    return result;
  }

  bool mem_commit(void *ptr, AllocationSize size)
  {
    int result = mprotect(ptr, rep(size), PROT_READ|PROT_WRITE);
    return result == 0;
  }

  void mem_decommit(void *ptr, AllocationSize size)
  {
    madvise(ptr, rep(size), MADV_DONTNEED);
    mprotect(ptr, rep(size), PROT_NONE);
  }

  void mem_release(void *ptr, AllocationSize size)
  {
    munmap(ptr, rep(size));
  }

  void* mem_reserve_large(AllocationSize size)
  {
    return mem_reserve(size);
  }

  bool mem_commit_large(void *ptr, AllocationSize size)
  {
    return mem_commit(ptr, size);
  }

  void mem_clear_working_set_pages()
  {
    //- brt: NYI.
  }

  ///////////////////////////////////////////////////////////////////////////////
  //~ brt: Threading

  Thread launch_thread(ThreadEntryPointFunctionType entry, void *data_p)
  {
    MacBackendData *data = mac_data();
    MacEntity *thread = mac_alloc_thread(data, entry, data_p);
    return os_thread(thread);
  }

  bool join_thread(Thread thread)
  {
    MacBackendData *data = mac_data();
    MacEntity *e = mac_thread(thread);
    int join_result = pthread_join(e->thread.handle, nullptr);
    mac_release_thread(data, thread);
    return join_result == 0;
  }

  ThreadID thread_id()
  {
    return ThreadID{ static_cast<uint32_t>(mach_thread_self()) };
  }

  ///////////////////////////////////////////////////////////////////////////////
  //~ brt: Synchronization primitives

  Mutex alloc_mutex()
  {
    MacBackendData *data = mac_data();
    return os_mutex(mac_mutex_alloc(data));
  }

  void release_mutex(Mutex mutex)
  {
    MacBackendData *data = mac_data();
    release_mac_mutex(data, mutex);
  }

  void lock_mutex(Mutex mutex)
  {
    MacEntity *e = mac_mutex(mutex);
    pthread_mutex_lock(&e->mutex);
  }

  void unlock_mutex(Mutex mutex)
  {
    MacEntity *e = mac_mutex(mutex);
    pthread_mutex_unlock(&e->mutex);
  }


  ConditionVariable alloc_condition_var()
  {
    MacBackendData *data = mac_data();
    MacEntity *cv = mac_condition_var_alloc(data);
    ConditionVariable result = ConditionVariable::Sentinel;
    if (cv != nullptr)
    {
      result = os_condition_var(cv);
    }
    return result;
  }
  
  void release_condition_var(ConditionVariable cv)
  {
    MacBackendData *data = mac_data();
    release_mac_condition_var(data, cv);
  }

  bool wait_condition_var(ConditionVariable cv, Mutex mutex, MicroSec end_us)
  {
    MacEntity* m_cv = mac_condition_var(cv);
    MacEntity* m_mutex = mac_mutex(mutex);
    int result;
    if (end_us == MicroSec::Infinite)
    {
      result = pthread_cond_wait(&m_cv->cv, &m_mutex->mutex);
    }
    else
    {
      timespec end_timespec{};
      end_timespec.tv_sec = rep(end_us)/Million(1);
      // Chop the seconds out.
      end_timespec.tv_nsec = Thousand(1) * (rep(end_us) - (rep(end_us) / Million(1)) * Million(1));
      result = pthread_cond_timedwait(&m_cv->cv, &m_mutex->mutex, &end_timespec);
    }
    return result != ETIMEDOUT;
  }

  void notify_one_condition_var(ConditionVariable cv)
  {
    MacEntity *m_cv = mac_condition_var(cv);
    pthread_cond_signal(&m_cv->cv);
  }

  void notify_all_condition_var(ConditionVariable cv)
  {
    MacEntity *m_cv = mac_condition_var(cv);
    pthread_cond_broadcast(&m_cv->cv);
  }

  void populate_core_render_data(RenderCoreData *rd_data)
  {
  }

  bool delta_meets_double_click_time(Ticks start, Ticks end)
  {
    if (start > end)
      return false;
    auto double_click_time = mac_data()->double_click_time;
    return ((rep(end) - rep(start)) <= rep(double_click_time));
  }

  bool delta_meets_double_click_time(Ticks32 start, Ticks32 end)
  {
    if (start > end)
      return false;
    auto double_click_time = mac_data()->double_click_time;
    return ((rep(end) - rep(start)) <= rep(double_click_time));
  }

  Error clipboard_text(Arena::Arena *arena, String8 *result)
  {
    *result = String8{};
    NSPasteboard *pboard = [NSPasteboard generalPasteboard];
    NSString *ns_string = [pboard stringForType:NSPasteboardTypeString];
    *result = str8_copy(arena, str8_cstr((char *)ns_string.UTF8String));
    [ns_string release];
    Error error = Error::None;
    return error;
  }

  Error set_clipboard(String8 buf)
  {
    NSString *ns_string = ns_string_from_str8(buf);
    NSPasteboard *pboard = [NSPasteboard generalPasteboard];
    [pboard clearContents];
    [pboard setString:ns_string forType:NSPasteboardTypeString];
    return Error::None;
  }

  Error set_clipboard_html(String8 buf, String8 html)
  {
    NSString *ns_buf_string = ns_string_from_str8(buf);
    NSString *ns_html_string = ns_string_from_str8(html);
    NSPasteboard *pboard = [NSPasteboard generalPasteboard];
    [pboard clearContents];
    [pboard setString:ns_buf_string forType:NSPasteboardTypeString];
    [pboard setString:ns_html_string forType:NSPasteboardTypeHTML];
    return Error::None;
  }

  ClipboardIdentity clipboard_id()
  {
    NSPasteboard *pboard = [NSPasteboard generalPasteboard];
    return ClipboardIdentity{ (uint64_t) [pboard changeCount] };
  }

  const SystemInfo *system_info()
  {
    MacBackendData *data = mac_data();
    return &data->sys_info;
  }

  //- brt: Time (in ms).
  Ticks get_ticks()
  {
    MacBackendData *data = mac_data();
    uint64_t now_ns = clock_gettime_nsec_np(CLOCK_REALTIME);
    uint64_t delta_ns = now_ns - data->start_time_ns;
    uint64_t delta_ms = delta_ns / Million(1);
    return static_cast<Ticks>(delta_ms);
  }

  Ticks32 get_ticks32()
  {
    return static_cast<Ticks32>(get_ticks());
  }

  MicroSec now_microseconds()
  {
    // brt: NOTE: this clock source cannot be changed or all of pthreads waits will not work...
    uint64_t ns = clock_gettime_nsec_np(CLOCK_REALTIME);
    uint64_t us = ns / 1000;
    return MicroSec{ us };
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

  void open_url_in_browser(String8 url)
  {
    auto scratch = Arena::scratch_begin(Arena::no_conflicts);
    String8 url_cstr = str8_copy(scratch.arena, url);
    CFStringRef s = CFStringCreateWithCString(NULL, url_cstr.str, kCFStringEncodingUTF8);
    CFURLRef cfurl = CFURLCreateWithString(NULL, s, NULL);
    LSOpenCFURLRef(cfurl, NULL);
    CFRelease(cfurl);
    CFRelease(s);
    Arena::scratch_end(scratch);
  }

  void open_path_in_explorer(String8 path)
  {
    auto scratch = Arena::scratch_begin(Arena::no_conflicts);
    String8 url_cstr = str8_copy(scratch.arena, path);
    CFStringRef s = CFStringCreateWithCString(0, url_cstr.str, kCFStringEncodingUTF8);
    CFURLRef cfurl = CFURLCreateWithFileSystemPath(0, s, kCFURLPOSIXPathStyle, false);
    if (cfurl)
    {
      CFArrayRef urls = CFArrayCreate(0, (const void **)&cfurl, 1, &kCFTypeArrayCallBacks);
      if (urls)
      {
        LSLaunchURLSpec spec = {0};
        spec.appURL = 0;
        spec.itemURLs = urls;
        spec.passThruParams = 0;
        spec.launchFlags = kLSLaunchDefaults | kLSLaunchAndDisplayErrors;
        spec.asyncRefCon = 0;
        LSOpenFromURLSpec(&spec, 0);
        CFRelease(urls);
      }
      LSOpenCFURLRef(cfurl, NULL);
    }
    CFRelease(cfurl);
    CFRelease(s);
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

  void post_thread_wakeup()
  {
    NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0x401
                                           data1:0
                                           data2:0];
    dispatch_async(dispatch_get_main_queue(),
    ^{
      [NSApp postEvent:event atStart:NO];
    });
  }

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

  void join_process(ProcessHandle handle)
  {
    pid_t pid = mac_process_handle(handle);
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) > 0)
    {
#if 0
      process_joined = 1;
      if (exit_code_out)
      {
        U64 exit_code = (U64)(WEXITSTATUS(status));
        *exit_code_out = exit_code;
      }
#endif
    }
  }

  void detach_process(ProcessHandle)
  {
    //- brt: no-op. not sure what's best to do, not as simple as on Windows
  }

  void terminate_process(ProcessHandle handle)
  {
    pid_t pid = mac_process_handle(handle);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }


  ScreenDimensions window_size()
  {
    MacBackendData *data = mac_data();
    CGFloat scale = data->wind.screen.backingScaleFactor;
    NSRect rect_pt = data->wind.frame;
    int width = (int)(rect_pt.size.width * scale);
    int height = (int)(rect_pt.size.height * scale);
    return { .width = Width{ width }, .height = Height{ height } };
  }

  ScreenDimensions client_size()
  {
    MacBackendData *data = mac_data();
    CGFloat scale = data->wind.screen.backingScaleFactor;
    NSRect rect_pt = data->wind.contentView.frame;
    int width = (int)(rect_pt.size.width * scale);
    int height = (int)(rect_pt.size.height * scale);
    return { .width = Width{ width }, .height = Height{ height } };
  }

  Vec4i window_rect(OSWindow wind)
  {
    OSMacWindow *ns_window = mac_window(wind);
    CGFloat scale = ns_window.screen.backingScaleFactor;
    NSRect rect_pt = ns_window.frame;
    int x = (int)(rect_pt.origin.x * scale);
    int y = (int)(rect_pt.origin.y * scale);
    int w = (int)(rect_pt.size.width * scale);
    int h = (int)(rect_pt.size.height * scale);
    return { x, y, w, h };
  }

#ifdef MAC_GFX
  void swap_buffers([[maybe_unused]] OSWindow wind)
  {
    MacBackendData* data = mac_data();
    GAP_UNUSED(data);
    // For now, assume we're swapping with the core window.
    assert(mac_window(wind) == data->wind);
    Render::os_swap_buffers(wind);
  }
#endif

  OSWindow core_window()
  {
    MacBackendData *data = mac_data();
    return os_window(data->wind);
  }

  Error apply_window_border_color(OSWindow wind, const Vec4f &color)
  {
    //- brt: NYI
    return Error::None;
  }

  Error apply_title_font_color(OSWindow wind, const Vec4f &color)
  {
    //- brt: NYI
    return Error::None;
  }

  void query_events(Arena::Arena* arena, Events* lst, Wait wait)
  {
    MacBackendData *data = mac_data();
    data->events_arena = arena;
    data->events = lst;

    if (data->events->count != 0)
    {
      wait = Wait::No;
    }

    @autoreleasepool
    {
      NSDate *deadline = is_yes(wait) ? [NSDate distantFuture] : [NSDate distantPast];
      NSEvent *ns_event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:deadline
                                                inMode:NSEventTrackingRunLoopMode
                                               dequeue:YES];
      for (;ns_event;)
      {
        bool should_send = true;
        OSMacWindow *window = data->wind;
        OSWindow window_handle = os_window(window);
        bool release = false;
        switch (ns_event.type)
        {
          // brt: wakeup event
          case NSEventTypeApplicationDefined:
          {
            push_event(EventSort::GapThreadWakeup, window_handle);
          }break;

          case NSEventTypeLeftMouseUp:
          case NSEventTypeRightMouseUp:
          {
            release = true;
          } // fallthrough
          case NSEventTypeLeftMouseDown:
          case NSEventTypeRightMouseDown:
          {
            Event *event = push_event(release ? EventSort::Release : EventSort::Press, window_handle);
            if (ns_event.type == NSEventTypeLeftMouseDown || ns_event.type == NSEventTypeLeftMouseUp)
            {
              event->key = Key::LeftMouseButton;
            }
            else if (ns_event.type == NSEventTypeRightMouseDown || ns_event.type == NSEventTypeRightMouseUp)
            {
              event->key = Key::RightMouseButton;
            }
            NSPoint pos = ns_event.locationInWindow;
            float scale_factor = ns_event.window.screen.backingScaleFactor;
            event->pos.x = (float)(pos.x * scale_factor);
            event->pos.y = (float)(ns_event.window.contentView.frame.size.height - pos.y) * scale_factor;
          }break;

          case NSEventTypeKeyUp:
          {
            release = true;
          } // fallthrough
          case NSEventTypeKeyDown:
          {
            should_send = false;
            Event *event = push_event(release ? EventSort::Release : EventSort::Press, window_handle);
            event->key = os_mac_os_key_from_vkey(ns_event.keyCode);
            event->is_repeat = ns_event.ARepeat != 0;
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
          }break;

          case NSEventTypeLeftMouseDragged:
          case NSEventTypeRightMouseDragged:
          {
            Event *event = push_event(EventSort::Press, window_handle);
            NSPoint pos = ns_event.locationInWindow;
            float scale_factor = ns_event.window.screen.backingScaleFactor;
            event->pos.x = (float)(pos.x * scale_factor);
            event->pos.y = (float)(ns_event.window.contentView.frame.size.height - pos.y) * scale_factor;
            if (ns_event.type == NSEventTypeLeftMouseDragged) 
            {
              event->key = Key::LeftMouseButton;
            }
            else if (ns_event.type == NSEventTypeRightMouseDragged) 
            {
              event->key = Key::RightMouseButton;
            }
          } // fallthrough
          case NSEventTypeMouseMoved:
          {
            Event *event = push_event(EventSort::MouseMove, window_handle);
            NSPoint pos = ns_event.locationInWindow;
            float scale_factor = ns_event.window.screen.backingScaleFactor;
            event->pos.x = (float)(pos.x * scale_factor);
            event->pos.y = (float)(ns_event.window.contentView.frame.size.height - pos.y) * scale_factor;
          }break;

          case NSEventTypeScrollWheel:
          {
            Event *event = push_event(EventSort::Scroll, window_handle);
            NSPoint pos = ns_event.locationInWindow;
            float wheel_x = ns_event.scrollingDeltaX;
            float wheel_y = ns_event.scrollingDeltaY;
            if (!ns_event.hasPreciseScrollingDeltas)
            {
              wheel_x *= 120.f;
              wheel_y *= 120.f;
            }
            float scale_factor = ns_event.window.screen.backingScaleFactor;
            event->pos.x = (float)(pos.x * scale_factor);
            event->pos.y = (float)(ns_event.window.contentView.frame.size.height - pos.y) * scale_factor;
            event->wheel_delta = Vec2f{ wheel_x, wheel_y };
          }break;
        }

        if (should_send)
        {
          [NSApp sendEvent:ns_event];
        }

        ns_event = [NSApp nextEventMatchingMask:NSEventMaskAny
          untilDate:[NSDate distantPast]
          inMode:NSEventTrackingRunLoopMode
          dequeue:YES];
      }
    }
  }

  void window_minimum_size(ScreenDimensions min_size)
  {
    //- brt: NYI
  }

  void destroy_window(OSWindow wind)
  {
    //- brt: NYI
  }

  bool window_minimized(OSWindow wind)
  {
    //- brt: NYI
    return false;
  }

  bool window_maximized(OSWindow wind)
  {
    //- brt: NYI
    return false;
  }

  bool window_fullscreened(OSWindow wind)
  {
    //- brt: NYI
    return false;
  }

  void window_maximize(OSWindow wind)
  {
    //- brt: NYI
  }

  void window_fullscreen(OSWindow wind)
  {
    //- brt: NYI
  }

  void window_restore(OSWindow wind)
  {
    //- brt: NYI
  }

  void window_windowed(OSWindow wind)
  {
    //- brt: NYI
  }

  Hz monitor_refresh_rate()
  {
    //- brt: NYI
    return Hz::Default;
  }

  Hz recompute_monitor_refresh_rate()
  {
    //- brt: NYI
    return Hz::Default;
  }

  DPI monitor_dpi()
  {
    MacBackendData *data = mac_data();
    CGFloat scale = data->wind.screen.backingScaleFactor;
    uint32_t dpi = (uint32_t)(96.0 * scale);
    return DPI{ dpi };
  }

  Error exe_path(Arena::Arena* arena, String8* buf)
  {
    auto scratch = Arena::scratch_begin({ &arena, 1 });

    int got_final_result = 0;
    char *buffer = 0;
    uint32_t size = 0;
    for(size_t cap = internal_max_path, r = 0; r < 4; cap *= 2, r += 1)
    {
      Arena::scratch_end(scratch);
      buffer = Arena::push_array<char>(scratch.arena, cap);
      if (_NSGetExecutablePath(buffer, &size) == 0)
      {
        got_final_result = 1;
        break;
      }
    }

    if(got_final_result && size > 0)
    {
      String8 full_name = str8(buffer, size);
      auto last_slash = str8_find_last_of(full_name, str8_mut(str8_literal("/")));
      if (last_slash != str8_index_sentinel)
      {
        String8 chopped = str8_substr(full_name, { .off = 0, .len = last_slash });
        *buf = str8_copy(arena, chopped);
      }
    }

    Arena::scratch_end(scratch);
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
    int mac_flags = 0;
    if(implies(access, FileAccess::Read | FileAccess::Write))
    {
      mac_flags = O_RDWR;
      mac_flags |= O_CREAT;
    }
    else if(implies(access, FileAccess::Write))
    {
      mac_flags = O_WRONLY;
      mac_flags |= O_CREAT;
      mac_flags |= O_TRUNC;
    }
    else if(implies(access, FileAccess::Read))
    {
      mac_flags = O_RDONLY;
    }

    if(implies(access, FileAccess::Append))
    {
      mac_flags |= O_APPEND;
      mac_flags |= O_CREAT;
    }

    auto scratch = Arena::scratch_begin(Arena::no_conflicts);
    // Need null-termination.
    String8 file_copy = str8_copy(scratch.arena, file);
    int fd = open(file_copy.str, mac_flags, 0755);
    Arena::scratch_end(scratch);
    FileHandle handle = FileHandle::Sentinel;
    if(fd != -1)
    {
      handle = os_file_handle(fd);
    }
    return handle;
  }

  void close_file(FileHandle handle)
  {
    assert(handle != FileHandle::Sentinel);
    close(mac_file_handle(handle));
  }

  FileLength file_length(FileHandle handle)
  {
    struct stat file_stat{};
    int res = fstat(mac_file_handle(handle), &file_stat);
    FileLength result = FileLength::Sentinel;
    if (res != -1)
    {
      result = FileLength(file_stat.st_size);
    }
    return result;
  }

  FileLength read_file(Arena::Arena *arena, String8 *buf, FileHandle handle, FileLength count)
  {
    int fd = mac_file_handle(handle);
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
    int fd = mac_file_handle(handle);
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
    String8 path_copy = str8_copy(scratch.arena, path);
    int result = stat(path_copy.str, &file_stat);
    Arena::scratch_end(scratch);
    FileProperties props{};
    if (result != -1)
    {
      props.size = FileLength(file_stat.st_size);
      props.created = dense_time_from_timespec(file_stat.st_ctimespec);
      props.modified = dense_time_from_timespec(file_stat.st_mtimespec);
      if (file_stat.st_mode & S_IFDIR)
      {
        props.props |= FileProperty::Directory;
      }
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
    char char_buf[internal_max_path];
    if (realpath(path.str, char_buf) == nullptr)
      return Error::CanonicalizeFailedToGetPathname;
    *buf = str8_copy(arena, str8_cstr(char_buf));
    return Error::None;
  }

  bool working_directory(Arena::Arena* arena, String8* buf)
  {
    char cwd_buf[internal_max_path];
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

  ///////////////////////////////////////////////////////////////////////////////
  //~ brt: Directory Iteration
  DirIter open_dir_iter(String8 dir, DirIterFlags flags)
  {
    MacBackendData *data = mac_data();
    DirIterData *dir_data = alloc_dir_iter_data(&data->dir_iter_data_alloc);
    dir_data->core_dir = sv_str8(dir);
    dir_data->dir = opendir(dir_data->core_dir.c_str());
    DirIter result = DirIter::Sentinel;
    if (dir_data->dir != nullptr)
    {
      dir_data->flags = flags;
      result = os_dir_iter(dir_data);
    }
    else
    {
      release_dir_iter_data(&data->dir_iter_data_alloc, dir_data);
    }
    return result;
  }

  bool dir_iter_next(Arena::Arena *arena, DirIterResult *result, DirIter iter)
  {
    assert(iter != DirIter::Sentinel);
    DirIterData *dir_data = mac_dir_iter(iter);
    if (dir_data->dir == nullptr ||
        implies(dir_data->flags, DirIterFlags::Done))
    {
      return false;
    }

    bool found = false;
    for (bool usable = false; !usable;)
    {
      dir_data->dp = readdir(dir_data->dir);
      found = dir_data->dp != nullptr;
      String8 name = str8_empty;

      FileProperty props{};
      if (found)
      {
        usable = true;
        auto scratch = Arena::scratch_begin({ &arena, 1 });
        name = str8_cstr(dir_data->dp->d_name);
        String8 dir_name = str8_cstr(dir_data->dp->d_name);
        String8 path = combine_paths(scratch.arena, str8_cppview(dir_data->core_dir), dir_name);
        result->path = path;
        result->props = file_properties(result->path);
        props = result->props.props;
        if (not implies(dir_data->flags, DirIterFlags::FullPath))
        {
          result->path = dir_name;
        }
        result->path = str8_copy(arena, result->path);
        Arena::scratch_end(scratch);
      }

      //- brt: figure out if this is filtered & excluse meta directories.
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

      if (usable)
      {
        break;
      }

      if (not found)
      {
        dir_data->flags |= DirIterFlags::Done;
        break;
      }
    }
    return found;
  }

  void close_dir_iter(DirIter iter)
  {
    assert(iter != DirIter::Sentinel);
    MacBackendData *data = mac_data();
    DirIterData *dir_data = mac_dir_iter(iter);
    release_dir_iter_data(&data->dir_iter_data_alloc, dir_data);
  }

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
    return OSError{ (uint32_t)errno };
  }
  
  String8 format_error(Arena::Arena *arena, OSError err)
  {
    String8 result{};
    String8 os_err_txt = str8_cstr(strerror(rep(err)));
    result = str8_copy(arena, os_err_txt);
    return result;
  }

} //namespace OS

@implementation OSMacWindow


- (void)windowWillClose:(NSNotification *)notification
{
  using namespace OS;
  MacBackendData *data = mac_data();
  OSMacWindow *window = data->wind;
  OSWindow window_handle = os_window(window);
  push_event(EventSort::Quit, window_handle);
}

- (void)flagsChanged:(NSEvent *)ns_event
{
  using namespace OS;
  MacBackendData *data = mac_data();
  OSMacWindow *window = data->wind;
  OSWindow window_handle = os_window(window);

  NSEventModifierFlags new_flags = [ns_event modifierFlags];
  NSEventModifierFlags old_flags = self.previous_flags;
  if ((new_flags & NSEventModifierFlagShift) != (old_flags & NSEventModifierFlagShift))
  {
    Event *event = push_event(!!(new_flags&NSEventModifierFlagShift) ? EventSort::Press : EventSort::Release, window_handle);
    event->key = Key::Shift;
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
  if ((new_flags & NSEventModifierFlagControl) != (old_flags & NSEventModifierFlagControl))
  {
    Event *event = push_event(!!(new_flags&NSEventModifierFlagControl) ? EventSort::Press : EventSort::Release, window_handle);
    event->key = Key::Ctrl;
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
  if ((new_flags & NSEventModifierFlagOption) != (old_flags & NSEventModifierFlagOption))
  {
    Event *event = push_event(!!(new_flags&NSEventModifierFlagOption) ? EventSort::Press : EventSort::Release, window_handle);
    event->key = Key::Alt;
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

  if ((new_flags & NSEventModifierFlagCommand) != (old_flags & NSEventModifierFlagCommand))
  {
    Event *event = push_event(!!(new_flags&NSEventModifierFlagCommand) ? EventSort::Press : EventSort::Release, window_handle);
    event->key = Key::Command;
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
    if (event->key == Key::Command && implies(event->modifiers, KeyMods::Cmd))
    {
      event->modifiers = remove_flag(event->modifiers, KeyMods::Cmd);
    }
  }

  if ((new_flags & NSEventModifierFlagCapsLock) != (old_flags & NSEventModifierFlagCapsLock))
  {
    Event *event = push_event(!!(new_flags&NSEventModifierFlagCapsLock) ? EventSort::Press : EventSort::Release, window_handle);
    event->key = Key::CapsLock;
  }
  self.previous_flags = new_flags;
  [super flagsChanged:ns_event];
}


- (NSDragOperation) draggingEntered:(id<NSDraggingInfo>) sender
{
  NSDragOperation result = NSDragOperationCopy;
  return result;
}

- (BOOL) performDragOperation:(id<NSDraggingInfo>) sender
{
  using namespace OS;
  MacBackendData *data = mac_data();
  OSMacWindow *window = data->wind;
  OSWindow window_handle = os_window(window);

  NSPasteboard *pboard = [sender draggingPasteboard];
  NSArray *types = pboard.types;
  if ([types containsObject:NSPasteboardTypeFileURL])
  {
    NSArray *classes = @[NSURL.class];
    NSDictionary *options = @{NSPasteboardURLReadingFileURLsOnlyKey:@YES};
    NSArray *file_urls = [pboard readObjectsForClasses:classes
                                               options:options];
    NSUInteger count = file_urls.count;
    if (count > 0)
    {
      DropFileList drop_list{};

      Event *event = push_event(EventSort::FileDrop, window_handle);
      NSPoint pos = sender.draggingLocation;
      float scale_factor = window.screen.backingScaleFactor;
      event->pos.x = (float)(pos.x * scale_factor);
      event->pos.y = (float)(window.contentView.frame.size.height - pos.y) * scale_factor;
      for EachIndex(idx, count)
      {
        NSURL *url = file_urls[idx];
        char *cstr = (char *)url.path.UTF8String;
        String8 str8_path = str8_copy(data->events_arena, str8_cstr(cstr));
        String8Node *node = Arena::push_array_no_zero<String8Node>(data->events_arena, 1);
        node->string = str8_path;
        SLLQueuePush(drop_list.first, drop_list.last, node);
        ++drop_list.count;
      }
      data->events->drop_files = drop_list;
      post_thread_wakeup();
    }
  }

  return YES;
}

@end

int main(int argc, char **argv)
{
  //- brt: Setup the system info.
  OS::MacBackendData *data = OS::mac_data();
  {
    data->sys_info.processor_count = OS::ProcCount(OS::sysctl_u64("hw.physicalcpu"));
    data->sys_info.page_size = OS::PageSize(OS::sysctl_u64("hw.pagesize"));
    data->sys_info.large_page_size = OS::PageSize{ MB(2) };
    data->sys_info.allocation_granularity = OS::AllocGranularity{ rep(data->sys_info.page_size) };
  }


  //- brt: Setup thread context.
  {
    Thread::TLD *tl_ctx = Thread::tld_alloc();
    Thread::tld_select(tl_ctx);
  }

  //- brt: Setup dynamic alloc state.
  {
    data->mac_arena = Arena::alloc(Arena::default_params);
  }

  //- brt: Setup computer name.
  {
    // man(2) gethostname:
    // > POSIX.1 guarantees that "Host names (not including the terminating null byte) are limited to HOST_NAME_MAX bytes".
    // Which implies we need to add an extra byte.
    constexpr int buf_size = 1024 + 1;
    char buf[buf_size];
    int result = gethostname(buf, buf_size);
    if (result == 0)
    {
      String8 str = str8_cstr(buf);
      // Copy into our arena.
      data->sys_info.machine_name = str8_copy(data->mac_arena, str);
    }
    // Note: if somehow the hostname violates the buffer size above, no host name for you, for now...
  }

  //- brt: Setup entity mutex.
  {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&data->mac_arena_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
  }

#ifdef BUILD_TRACK_ARENA
  Arena::init_tracker_arena();
#endif

  //- brt: Initialize other macOS data.
  init_mac(data);

  return gap_main_entry(argc, argv);
}
