#ifdef WIN32
#pragma warning(disable: 4244 4267 4459)
#else
#pragma GCC diagnostic ignored "-Wunused-function" // ftsdf.c:1365:3: warning: ‘sdf_shape_dump’ defined but not used
#ifndef __clang__ 
#pragma GCC diagnostic ignored "-Wdangling-pointer" // ftgrays.c:1886:20: warning: storing the address of local variable ‘buffer’ in ‘*worker.ycells’
#endif
#endif

// Config values.
#define _CRT_SECURE_NO_WARNINGS
#define _LIB
#define FT_DEBUG_LEVEL_ERROR
#define FT_DEBUG_LEVEL_TRACE
#define FT2_BUILD_LIBRARY
#define FT_CONFIG_OPTION_ERROR_STRINGS
#define FT_CONFIG_OPTION_SUBPIXEL_RENDERING

// These need to go first because there are macros which create extra stuff.
#include "src/smooth/smooth.c"
#undef ONE_PIXEL
#include "src/pfr/pfr.c"
#include "src/sdf/sdf.c"

#include "src/autofit/autofit.c"
#include "src/base/ftbase.c"
#include "src/base/ftbbox.c"
#include "src/base/ftbdf.c"
#include "src/base/ftbitmap.c"
#include "src/base/ftcid.c"
#include "src/base/ftfstype.c"
#include "src/base/ftgasp.c"
#include "src/base/ftglyph.c"
#include "src/base/ftgxval.c"
#include "src/base/ftinit.c"
#include "src/base/ftmm.c"
#include "src/base/ftotval.c"
#include "src/base/ftpatent.c"
#include "src/base/ftpfr.c"
#include "src/base/ftstroke.c"
#include "src/base/ftsynth.c"
#include "src/base/fttype1.c"
#include "src/base/ftwinfnt.c"
#include "src/bdf/bdf.c"
#include "src/cache/ftcache.c"
#include "src/cff/cff.c"
#include "src/cid/type1cid.c"
#include "src/dlg/dlgwrap.c"
#include "src/gzip/ftgzip.c"
#include "src/lzw/ftlzw.c"
#include "src/pcf/pcf.c"
#include "src/psaux/psaux.c"
#include "src/pshinter/pshinter.c"
#include "src/psnames/psmodule.c"
#include "src/raster/raster.c"
#include "src/sfnt/sfnt.c"
#include "src/svg/svg.c"
#include "src/truetype/truetype.c"
#include "src/type1/type1.c"
#include "src/type42/type42.c"
#include "src/winfonts/winfnt.c"
#include "src/base/ftdebug.c"
#include "src/base/ftsystem.c"
