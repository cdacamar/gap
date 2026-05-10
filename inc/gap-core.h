#pragma once

#include "macros.h"

int gap_main_entry(int argc, char** argv);

// For global frame updates e.g. via an event loop.
struct RenderCoreData;
void update_frame(RenderCoreData* data);

// Basic units (borrowed from RADDBG).
// Note: Let's keep these as macros for now, I see no reason to introduce a function overhead for such
// a small op.
#define KB(n)  ((static_cast<uint64_t>(n)) << 10)
#define MB(n)  ((static_cast<uint64_t>(n)) << 20)
#define GB(n)  ((static_cast<uint64_t>(n)) << 30)
#define TB(n)  ((static_cast<uint64_t>(n)) << 40)

#define Thousand(n)   ((n)*1000)
#define Million(n)    ((n)*1000000)
#define Billion(n)    ((n)*1000000000)

#define BUILD_TITLE "gap - Text Diff"

#define BUILD_MAJOR 0
#define BUILD_MINOR 2
#define BUILD_PATCH 0

#define VERSION_STRING STRINGIFY(BUILD_MAJOR) "." STRINGIFY(BUILD_MINOR) "." STRINGIFY(BUILD_PATCH)

#if BUILD_DEBUG
#define BUILD_MODE_STRING "[debug]"
#else
#define BUILD_MODE_STRING "[release]"
#endif // BUILD_DEBUG

#ifdef BUILD_PROFILED
#define BUILD_PROFILED_STRING " [profiling]"
#else
#define BUILD_PROFILED_STRING
#endif // BUILD_PROFILED

// OS / Rendering info.
#ifdef WIN32
#define BUILD_OS_STRING "WIN32"
#endif // WIN32

#ifdef OS_LINUX
#define BUILD_OS_STRING "LINUX"
#endif // OS_LINUX

#ifdef BUILD_OPENGL_RENDERER
#define BUILD_RENDERER_STRING "OpenGL"
#endif // BUILD_OPENGL_RENDERER

#ifdef BUILD_D3D11_RENDERER
#define BUILD_RENDERER_STRING "DX11"
#endif // BUILD_D3D11_RENDERER

#define BUILD_COMBINED_STRING BUILD_MODE_STRING BUILD_PROFILED_STRING

#define ALPHA_BRIEF "(alpha)"

#define HUMAN_VERSION_STRING "Version " VERSION_STRING " " ALPHA_BRIEF
#define HUMAN_BUILD_STRING __DATE__ " [" BUILD_GIT_HASH "] " BUILD_COMBINED_STRING
#define HUMAN_CONFIG_STRING "[" BUILD_OS_STRING "]" " [" BUILD_RENDERER_STRING "]"

#define ALPHA_NOTE "NOTE: This version of gap is highly experimental.\n"   \
                   "There will be bugs, there are missing features, and\n" \
                   "you may be disappointed!\n"                            \
                   "\n"                                                    \
                   "Thank you all for taking the time to try out my pet\n" \
                   "project, gap!\n"                                       \
                   "\n"                                                    \
                   "<3 - starfreakclone"

#define BUILD_ISSUES_LINK_HUMAN BUILD_TITLE " Discord | bugs"
#define BUILD_ISSUES_LINK "https://discord.com/channels/1327544172818726962/1327549015759257621"