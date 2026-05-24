#pragma once

// Helper macros for enabling various compiler warnings.

#ifdef WIN32
#define ENABLE_UNHANDLED_CASE_WARNING() _Pragma("warning(push)") _Pragma("warning(error: 4061)") _Pragma("warning(error: 4062)")
#define DISABLE_UNHANDLED_CASE_WARNING() _Pragma("warning(pop)")

#define ENABLE_ANON_FUNCTION_REMOVED_WARNING() _Pragma("warning(push)") _Pragma("warning(error: 5245)")
#define DISABLE_ANON_FUNCTION_REMOVED_WARNING() _Pragma("warning(pop)")

#define SUPPRESS_NONSTANDARD_EXTENSION_WARNING() _Pragma("warning(push)") _Pragma("warning(disable: 4201 4200)")
#define ENABLE_NONSTANDARD_EXTENSION_WARNING() _Pragma("warning(pop)")

#define SUPPRESS_MULTI_LINE_COMMENT_WARNING() _Pragma("warning(push)") _Pragma("warning(disable: 5333)")
#define ENABLE_MULTI_LINE_COMMENT_WARNING() _Pragma("warning(pop)")

#define SUPPRESS_IF_CONSTEXPR_SUGGEST_WARNING() _Pragma("warning(push)") _Pragma("warning(disable: 4127)")
#define ENABLE_IF_CONSTEXPR_SUGGEST_WARNING() _Pragma("warning(pop)")

#define SUPPRESS_MEMSET_NON_TRIVIAL_WARNING()
#define ENABLE_MEMSET_NON_TRIVIAL_WARNING()
#else // ^^^ WIN32 ^^^ / vvv !WIN32 vvv
#define ENABLE_UNHANDLED_CASE_WARNING() _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic warning \"-Wswitch-enum\"") _Pragma("GCC diagnostic warning \"-Wswitch\"")
#define DISABLE_UNHANDLED_CASE_WARNING() _Pragma("GCC diagnostic pop")

#define ENABLE_ANON_FUNCTION_REMOVED_WARNING()
#define DISABLE_ANON_FUNCTION_REMOVED_WARNING()

#define SUPPRESS_NONSTANDARD_EXTENSION_WARNING()
#define ENABLE_NONSTANDARD_EXTENSION_WARNING()

#define SUPPRESS_MULTI_LINE_COMMENT_WARNING() _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wcomment\"")
#define ENABLE_MULTI_LINE_COMMENT_WARNING() _Pragma("GCC diagnostic pop")

#define SUPPRESS_IF_CONSTEXPR_SUGGEST_WARNING()
#define ENABLE_IF_CONSTEXPR_SUGGEST_WARNING()

#if !defined(__clang__)
#define SUPPRESS_MEMSET_NON_TRIVIAL_WARNING() _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wclass-memaccess\"")
#define ENABLE_MEMSET_NON_TRIVIAL_WARNING() _Pragma("GCC diagnostic pop")
#else
#define SUPPRESS_MEMSET_NON_TRIVIAL_WARNING()
#define ENABLE_MEMSET_NON_TRIVIAL_WARNING()
#endif

#endif // WIN32

#ifdef WIN32
#pragma section(".rdata$", read)
#define read_only __declspec(allocate(".rdata$"))
#else // ^^^ WIN32 ^^^ / vvv !WIN32 vvv
// NOTE: On gcc, the .rodata section is not particularly functional.  It will work
// on LLVM-based toolchains, so make it empty for now.
//#define read_only __attribute__((section(".rodata")))
#define read_only
#endif // WIN32

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#ifdef WIN32
#define PATH_SEP "\\"
#else // ^^^ WIN32 ^^^ / vvv !WIN32 vvv
#define PATH_SEP "/"
#endif // WIN32

#ifdef BUILD_PROFILED
#define PROF_FRAME_START() FrameMark
#define PROF_FRAME_END()

#define PROF_SCOPE() ZoneScoped
#define PROF_NAME_SCOPE(fmt, ...) ZoneNameF(fmt, __VA_ARGS__)
#define PROF_BEGIN(ctx_name, scope_name) TracyCZone(ctx_name, 1); TracyCZoneName(ctx_name, scope_name, std::size(scope_name) - 1)
#define PROF_END(ctx_name) TracyCZoneEnd(ctx_name)

// For enabling tracy.
#define TRACY_ENABLE
#include "tracy/Tracy.hpp"
#include "tracy/TracyC.h"
#else // ^^^ BUILD_PROFILED ^^^ / vvv !BUILD_PROFILED vvv
#define PROF_FRAME_START()
#define PROF_FRAME_END()

#define PROF_SCOPE()
#define PROF_NAME_SCOPE(fmt, ...)
#define PROF_BEGIN(ctx_name, scope_name)
#define PROF_END(ctx_name)
#endif

// ASAN detection / macros.
#ifdef __SANITIZE_ADDRESS__
#define ASAN_ENABLED 1
#else
#define ASAN_ENABLED 0
#endif // __SANITIZE_ADDRESS__

#if ASAN_ENABLED
extern "C" void __asan_poison_memory_region(void const volatile* addr, size_t size);
extern "C" void __asan_unpoison_memory_region(void const volatile* addr, size_t size);
#define ASAN_POISON_MEMORY_REGION(addr, size)   __asan_poison_memory_region((addr), (size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) __asan_unpoison_memory_region((addr), (size))
#else // ^^^ ASAN_ENABLED ^^^ / vvv !ASAN_ENABLED vvv
#define ASAN_POISON_MEMORY_REGION(addr, size)
#define ASAN_UNPOISON_MEMORY_REGION(addr, size)
#endif

#if BUILD_DEBUG
#define GAP_UNUSED(x)
#else
#define GAP_UNUSED(x) (void)(x)
#endif // BUILD_DEBUG

#define GAP_UNUSED_RESULT(x) (void)(x)

// Data structure macros (mostly borrowed from RADDBG).
#define CheckNil(nil,p) ((p) == 0 || (p) == nil)
#define SetNil(nil,p) ((p) = nil)

// Helpers for walking.
#define EachIndex(it, count) (uint64_t it = 0; it < (count); it += 1)
#define EachNode(it, first) (auto *it = first; it != 0; it = it->next)
#define EachNode_N(nil, it, first) (auto *it = first, *z = (nil); it != z; it = it->next)

// Stacks.
#define SLLStackPush_N(f,n,next) ((n)->next=(f), (f)=(n))
#define SLLStackPop_N(f,next) ((f)=(f)->next)

// Doubly-linked list.
#define DLLInsert_NPZ(nil,f,l,p,n,next,prev) (CheckNil(nil,f) ? \
((f) = (l) = (n), SetNil(nil,(n)->next), SetNil(nil,(n)->prev)) :\
CheckNil(nil,p) ? \
((n)->next = (f), (f)->prev = (n), (f) = (n), SetNil(nil,(n)->prev)) :\
((p)==(l)) ? \
((l)->next = (n), (n)->prev = (l), (l) = (n), SetNil(nil, (n)->next)) :\
(((!CheckNil(nil,p) && CheckNil(nil,(p)->next)) ? (0) : ((p)->next->prev = (n))), ((n)->next = (p)->next), ((p)->next = (n)), ((n)->prev = (p))))
#define DLLPushBack_NPZ(nil,f,l,n,next,prev) DLLInsert_NPZ(nil,f,l,l,n,next,prev)
#define DLLPushFront_NPZ(nil,f,l,n,next,prev) DLLInsert_NPZ(nil,l,f,f,n,prev,next)
#define DLLRemove_NPZ(nil,f,l,n,next,prev) (((n) == (f) ? (f) = (n)->next : (0)),\
((n) == (l) ? (l) = (l)->prev : (0)),\
(CheckNil(nil,(n)->prev) ? (0) :\
((n)->prev->next = (n)->next)),\
(CheckNil(nil,(n)->next) ? (0) :\
((n)->next->prev = (n)->prev)))

// Singly-linked, doubly-headed lists (queues)
#define SLLQueuePush_NZ(nil,f,l,n,next) (CheckNil(nil,f)?\
((f)=(l)=(n),SetNil(nil,(n)->next)):\
((l)->next=(n),(l)=(n),SetNil(nil,(n)->next)))
#define SLLQueuePushFront_NZ(nil,f,l,n,next) (CheckNil(nil,f)?\
((f)=(l)=(n),SetNil(nil,(n)->next)):\
((n)->next=(f),(f)=(n)))
#define SLLQueuePop_NZ(nil,f,l,next) ((f)==(l)?\
(SetNil(nil,f),SetNil(nil,l)):\
((f)=(f)->next))

// Doubly-linked-list helpers
#define DLLInsert_NP(f,l,p,n,next,prev) DLLInsert_NPZ(0,f,l,p,n,next,prev)
#define DLLPushBack_NP(f,l,n,next,prev) DLLPushBack_NPZ(0,f,l,n,next,prev)
#define DLLPushFront_NP(f,l,n,next,prev) DLLPushFront_NPZ(0,f,l,n,next,prev)
#define DLLRemove_NP(f,l,n,next,prev) DLLRemove_NPZ(0,f,l,n,next,prev)
#define DLLInsert(f,l,p,n) DLLInsert_NPZ(0,f,l,p,n,next,prev)
#define DLLPushBack(f,l,n) DLLPushBack_NPZ(0,f,l,n,next,prev)
#define DLLPushFront(f,l,n) DLLPushFront_NPZ(0,f,l,n,next,prev)
#define DLLRemove(f,l,n) DLLRemove_NPZ(0,f,l,n,next,prev)

// Singly-linked, doubly-headed list helpers
#define SLLQueuePush_N(f,l,n,next) SLLQueuePush_NZ(0,f,l,n,next)
#define SLLQueuePushFront_N(f,l,n,next) SLLQueuePushFront_NZ(0,f,l,n,next)
#define SLLQueuePop_N(f,l,next) SLLQueuePop_NZ(0,f,l,next)
#define SLLQueuePush(f,l,n) SLLQueuePush_NZ(0,f,l,n,next)
#define SLLQueuePushFront(f,l,n) SLLQueuePushFront_NZ(0,f,l,n,next)
#define SLLQueuePop(f,l) SLLQueuePop_NZ(0,f,l,next)

// Singly-linked, singly-headed list helpers
#define SLLStackPush(f,n) SLLStackPush_N(f,n,next)
#define SLLStackPop(f) SLLStackPop_N(f,next)
