#pragma once

#include "arena.h"
#include "constants.h"
#include "gap-core.h"
#include "gap-strings.h"
#include "types.h"
#include "vec.h"

// Atomics.
// These are implemented as macros to remove function call overhead which might interrupt atomicity.
// Largely borrowed from RADDBG.
#ifdef WIN32
# include <intrin.h>
#define os_atomic_u128_eval(x, r)              (bool)(memset((r), 0, sizeof(__int64)*2), _InterlockedCompareExchange128((__int64 *)(x), 0, 0, (__int64 *)(r)))
#define os_atomic_u128_eval_cond_assign(x,k,c) (bool)_InterlockedCompareExchange128((__int64 *)(x), ((__int64 *)&(k))[1], ((__int64 *)&(k))[0], (__int64 *)(c))
#define os_atomic_u64_eval(x)                  __iso_volatile_load64((volatile __int64 *)(x))
#define os_atomic_u64_inc_eval(x)              _InterlockedIncrement64((__int64 *)(x))
#define os_atomic_u64_dec_eval(x)              _InterlockedDecrement64((__int64 *)(x))
#define os_atomic_u64_eval_assign(x,c)         _InterlockedExchange64((__int64 *)(x),(c))
#define os_atomic_u64_add_eval(x,c)            _InterlockedAdd64((__int64 *)(x), c)
#define os_atomic_u64_eval_cond_assign(x,k,c)  _InterlockedCompareExchange64((__int64 *)(x),(__int64)(k),(__int64)(c))
#define os_atomic_u32_eval(x)                  __iso_volatile_load32((volatile int *)(x))
#define os_atomic_u32_inc_eval(x)              _InterlockedIncrement((long *)(x))
#define os_atomic_u32_dec_eval(x)              _InterlockedDecrement((long *)(x))
#define os_atomic_u32_eval_assign(x,c)         _InterlockedExchange((long *)(x),(c))
#define os_atomic_u32_eval_cond_assign(x,k,c)  _InterlockedCompareExchange((long *)(x),(k),(c))
#define os_atomic_u32_add_eval(x,c)            _InterlockedAdd((long *)(x), (c))
#elif defined(OS_LINUX)
#define os_atomic_u128_eval(x, r)              ({ __int128 _new = __atomic_load_n((__int128*)x, __ATOMIC_SEQ_CST); memcpy(r, &_new, sizeof(__int128)); r; })
#define os_atomic_u128_eval_cond_assign(x,k,c) ({ __int128 _new; memcpy(&_new, &(k), sizeof(__int128)); bool _res = __atomic_compare_exchange_n((__int128 *)(x),(__int128*)(c),_new,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST); memcpy(&(k), &_new, sizeof(__int128)); _res; })
#define os_atomic_u64_eval(x)                  __atomic_load_n(x, __ATOMIC_SEQ_CST)
#define os_atomic_u64_inc_eval(x)              (__atomic_fetch_add((uint64_t *)(x), 1, __ATOMIC_SEQ_CST) + 1)
#define os_atomic_u64_dec_eval(x)              (__atomic_fetch_sub((uint64_t *)(x), 1, __ATOMIC_SEQ_CST) - 1)
#define os_atomic_u64_eval_assign(x,c)         __atomic_exchange_n(x, c, __ATOMIC_SEQ_CST)
#define os_atomic_u64_add_eval(x,c)            (__atomic_fetch_add((uint64_t *)(x), c, __ATOMIC_SEQ_CST) + (c))
#define os_atomic_u64_eval_cond_assign(x,k,c)  ({ uint64_t _new = (c); __atomic_compare_exchange_n((uint64_t *)(x),&_new,(k),0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST); _new; })
#define os_atomic_u32_eval(x)                  __atomic_load_n(x, __ATOMIC_SEQ_CST)
#define os_atomic_u32_inc_eval(x)              (__atomic_fetch_add((uint32_t *)(x), 1, __ATOMIC_SEQ_CST) + 1)
#define os_atomic_u32_dec_eval(x)              (__atomic_fetch_sub((uint32_t *)(x), 1, __ATOMIC_SEQ_CST) - 1)
#define os_atomic_u32_add_eval(x,c)            (__atomic_fetch_add((uint32_t *)(x), c, __ATOMIC_SEQ_CST) + (c))
#define os_atomic_u32_eval_assign(x,c)         __atomic_exchange_n((x), (c), __ATOMIC_SEQ_CST)
#define os_atomic_u32_eval_cond_assign(x,k,c)  ({ uint32_t _new = (c); __atomic_compare_exchange_n((uint32_t *)(x),&_new,(k),0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST); _new; })
#else
#error Atomic intrinsics not defined for this compiler / architecture.
#endif

namespace OS
{
    enum class Error
    {
        None,
        ClipboardTextUnavailable,
        UnableToOpenClipboard,
        InvalidClipboardData,
        EmptyClipboardFailed,
        BufferTruncated,
        CouldNotLocatePrefPath,
        CreateDirectoryFailed,
        FailedToApplyWindowCaptionColor,
        FailedToApplyWindowFontColor,
        CanonicalizeFailedToOpenFile,
        CanonicalizeFailedToGetPathname,
        Count
    };

    enum class OSWindow : uint64_t
    {
        Sentinel = sentinel_for<OSWindow>
    };

    enum class ProcCount : uint32_t { };
    enum class PageSize : uint64_t { };
    enum class AllocGranularity : uint64_t { };

    struct SystemInfo
    {
        ProcCount processor_count;
        PageSize page_size;
        PageSize large_page_size;
        AllocGranularity allocation_granularity;
        String8 machine_name;
    };

    struct Event
    {
        Event* next;
        OSWindow window;
        EventSort sort;
        Key key;
        Vec2f pos;
        Vec2f wheel_delta;
        uint32_t repeat_count;
        uint32_t character;
        CursorStyle new_cursor;
        KeyMods modifiers;
        bool right_sided;
        bool is_repeat;
    };

    // In milliseconds.
    enum class Ticks : uint64_t { };
    // In milliseconds (32-bits).
    enum class Ticks32 : uint32_t { };

    enum class OSError : uint32_t
    {
        Success
    };

    struct DropFileList
    {
        String8Node* first;
        String8Node* last;
        uint64_t count;
    };

    struct Events
    {
        Event* first;
        Event* last;
        uint64_t count;
        DropFileList drop_files;
    };

    enum class Wait : bool { No, Yes };

    enum class ClipboardIdentity : uint64_t
    {
        Sentinel = sentinel_for<ClipboardIdentity>
    };

    enum class FileHandle : uint64_t
    {
        Sentinel = sentinel_for<FileHandle>
    };

    enum class FileAccess : uint32_t
    {
        None       = 0,
        Read       = 1U << 1,
        Write      = 1U << 2,
        Execute    = 1U << 3,
        ShareRead  = 1U << 4,
        ShareWrite = 1U << 5,
        Append     = 1U << 6,
        Inherited  = 1U << 7,
        IsDir      = 1U << 8,
    };

    enum class FileLength : uint64_t
    {
        Sentinel = sentinel_for<FileLength>
    };

    enum class FileOffset : uint64_t { };

    enum class BytesWritten : uint64_t { };

    enum class FileProperty : uint32_t
    {
        None      = 0,
        Directory = 1 << 0,
    };

    struct FileProperties
    {
        FileLength size;
        DenseTime modified;
        DenseTime created;
        FileProperty props;
    };

    enum class DirIterFlags : uint32_t
    {
        None      = 0,
        SkipDirs  = 1U << 0,
        SkipFiles = 1U << 1,
        FullPath  = 1U << 2,

        Done      = 1U << 31
    };

    enum class DirIter : uint64_t
    {
        Sentinel = sentinel_for<DirIter>
    };

    struct DirIterResult
    {
        String8 path;
        FileProperties props;
    };

    enum class LibraryHandle : uint64_t
    {
        Sentinel = sentinel_for<LibraryHandle>
    };

    enum class LaunchFlags : uint32_t
    {
        None        = 0,
        InheritEnv  = 1U << 0,
        Consoleless = 1U << 1,
        Suspend     = 1U << 2,
        CLIProcess  = 1U << 3, // As if by calling native OS 'cmd ...'.
    };

    struct ProcessLaunchParams
    {
        String8 wd;
        String8List cmd_line;
        String8List env;
        FileHandle stdout_file = FileHandle::Sentinel;
        FileHandle stdin_file = FileHandle::Sentinel;
        FileHandle stderr_file = FileHandle::Sentinel;
        LaunchFlags flags;
    };

    enum class ProcessHandle : uint64_t
    {
        Sentinel = sentinel_for<ProcessHandle>
    };

    enum class IOPipe : uint64_t
    {
        Sentinel = sentinel_for<IOPipe>
    };

    struct ReadIOPipeInput
    {
        Arena::Arena* arena;
        String8* std_out;
        String8* std_err;
    };

    struct ReadIOPipeResult
    {
        int exit_code;
        bool pending_output;
        bool std_out_write;
        bool std_err_write;
    };

    enum class AllocationSize : uint64_t { };

    enum class Mutex : uint64_t
    {
        Sentinel = sentinel_for<Mutex>
    };

    enum class ConditionVariable : uint64_t
    {
        Sentinel = sentinel_for<ConditionVariable>
    };

    enum class MicroSec : uint64_t
    {
        Infinite = sentinel_for<MicroSec>
    };

    enum class ThreadID : uint32_t { };

    enum class Thread : uint64_t
    {
        Sentinel = sentinel_for<Thread>
    };

    inline constexpr Vec4i default_window_pos = { Constants::max_S32 };

    using ThreadEntryPointFunctionType = void(*)(void*);

    // Date/time
    DenseTime dense_time_from_date_time(const DateTime& date_time);
    DateTime date_time_from_micro_seconds(MicroSec us);

    // Error info.
    String8 error_text(Error e);
    OSError last_os_error_code();
    String8 format_error(Arena::Arena* arena, OSError err);

    // Windowing.
    OSWindow init_window(Vec4i wind_rect, String8 title);
    void window_minimum_size(ScreenDimensions min_size);
    void destroy_window(OSWindow wind);
    bool window_minimized(OSWindow wind);
    bool window_maximized(OSWindow wind);
    void window_maximize(OSWindow wind);
    void window_restore(OSWindow wind);
    bool window_fullscreened(OSWindow wind);
    void window_fullscreen(OSWindow wind);
    void window_windowed(OSWindow wind);
    Vec4i window_rect(OSWindow wind);
    void swap_buffers(OSWindow wind);
    OSWindow core_window();
    Error apply_window_border_color(OSWindow wind, const Vec4f& color);
    Error apply_title_font_color(OSWindow wind, const Vec4f& color);

    // Event processing.
    // Note: This API will append more events to 'lst'.
    void query_events(Arena::Arena* arena, Events* lst, Wait wait);
    String8 event_sort_string(EventSort s);
    String8 key_to_string(Key k);

    // Direct Interaction.
    // Mouse cursor.
    void set_cursor(CursorStyle style);
    bool delta_meets_double_click_time(Ticks start, Ticks end);
    bool delta_meets_double_click_time(Ticks32 start, Ticks32 end);

    // Clipboard.
    Error clipboard_text(Arena::Arena* arena, String8* result);
    Error set_clipboard(String8 buf);
    Error set_clipboard_html(String8 buf, String8 html);
    ClipboardIdentity clipboard_id();

    // Queries.
    // System information.
    const SystemInfo* system_info();

    // Time (in ms).
    Ticks get_ticks();
    Ticks32 get_ticks32();
    MicroSec now_microseconds();

    // Monitor.
    Hz monitor_refresh_rate();
    Hz recompute_monitor_refresh_rate();
    ScreenDimensions window_size();
    ScreenDimensions client_size();
    DPI monitor_dpi();

    // Paths / File IO.
    Error exe_path(Arena::Arena* arena, String8* buf);
    Error app_path(Arena::Arena* arena, String8* buf, String8 org, String8 app);
    FileHandle open_file(String8 file, FileAccess access);
    void close_file(FileHandle handle);
    FileLength file_length(FileHandle handle);
    FileLength read_file(Arena::Arena* arena, String8* buf, FileHandle handle, FileLength count);
    BytesWritten write_file(FileHandle handle, FileOffset off, String8 buf);
    FileProperties file_properties(String8 path);
    Error create_directory(String8 dir);
    bool directory_exists(String8 dir);
    bool file_or_path_exists(String8 path);
    bool regular_file_exists(String8 file);
    Error canonical_file_path(Arena::Arena* arena, String8* buf, String8 path);
    bool working_directory(Arena::Arena* arena, String8* buf);
    bool set_working_directory(String8 path);

    // Directory iteration.
    DirIter open_dir_iter(String8 dir, DirIterFlags flags);
    bool dir_iter_next(Arena::Arena* arena, DirIterResult* result, DirIter iter);
    void close_dir_iter(DirIter iter);

    // Path construction.
    String8 parent_path(String8 path);
    String8 combine_paths(Arena::Arena* arena, String8 a, String8 b);
    String8 file_extension(String8 file);
    String8 root_of_path(String8 path);

    // External command invocation.
    void open_url_in_browser(String8 url);
    void open_path_in_explorer(String8 path);
    void open_path_in_preferred_explorer(String8 explorer, String8 path);

    // Gap-specific events.
    void post_thread_wakeup();

    // Library handling.
    LibraryHandle load_library(String8 lib_name);
    void unload_library(LibraryHandle lib);
    void* get_function(LibraryHandle lib, String8 fn);
    LibraryHandle get_gl_library();
    void* get_gl_function(LibraryHandle gl_lib, String8 fn);

    // Process creation.
    ProcessHandle launch_process(const ProcessLaunchParams& in);
    IOPipe launch_piped_process(const ProcessLaunchParams& in);
    ReadIOPipeResult read_piped_process(IOPipe piped_process, ReadIOPipeInput in);
    void join_process(ProcessHandle handle);
    void detach_process(ProcessHandle handle);
    void terminate_process(ProcessHandle handle);
    void close_pipes_and_terminate_process(IOPipe piped_process);

    // Memory allocation.
    void* mem_reserve(AllocationSize size);
    bool mem_commit(void* ptr, AllocationSize size);
    void mem_decommit(void* ptr, AllocationSize size);
    void mem_release(void* ptr, AllocationSize size);
    void* mem_reserve_large(AllocationSize size);
    bool mem_commit_large(void* ptr, AllocationSize size);
    void mem_clear_working_set_pages();

    // Threading.
    Thread launch_thread(ThreadEntryPointFunctionType entry, void* data_p);
    bool join_thread(Thread thread);
    void thread_entry_bridge(ThreadEntryPointFunctionType target, void* data_p);
    ThreadID thread_id();

    // Synchronization primitives.
    // Mutex.
    Mutex alloc_mutex();
    void release_mutex(Mutex mutex);
    void lock_mutex(Mutex mutex);
    void unlock_mutex(Mutex mutex);

    // Condition variable.
    ConditionVariable alloc_condition_var();
    void release_condition_var(ConditionVariable cv);
    // Note: 'end_us' is an exact time, so typically you want 'now_microseconds() + x'.
    bool wait_condition_var(ConditionVariable cv, Mutex mutex, MicroSec end_us);
    void notify_one_condition_var(ConditionVariable cv);
    void notify_all_condition_var(ConditionVariable cv);

    // Setup for core rendering.
    void populate_core_render_data(RenderCoreData* data);
} // namespace OS