#!/bin/bash
set -eu
cd `dirname "$0"`

for arg in "$@";
do
  declare $arg='1'
done

# --- Unpack Arguments -------------------------------------------------------
if [ -z "${gcc+x}" ];                     then clang=1; fi
if [ -z "${release+x}" ];                 then debug=1; fi
if [ -n "${debug+x}" ];                   then unset release; echo "[debug mode]"; fi
if [ -n "${release+x}" ];                 then unset debug;   echo "[release mode]"; fi
if [ -n "${gcc+x}" ];                     then unset clang;   compiler="gcc";   echo "[gcc compile]"; fi
if [ -n "${clang+x}" ];                   then unset gcc;     compiler="clang"; echo "[clang compile]"; fi
if [ $# -eq 0 ];                          then gap=1;         echo '[default mode, assuming `gap` build]'; fi
if [ $# -eq 1 ] && [ -n "${release+x}" ]; then gap=1;         echo '[default mode, assuming `gap` build]'; fi

# --- Unpack XCode Arguments -------------------------------------------------
if [ "${ENABLE_ADDRESS_SANITIZER:-NO}" == "YES" ]; then asan=1; fi

# --- Unpack Command Line Build Arguments ------------------------------------
build_assets=0
metal_renderer=1
auto_compile_flags=""

if [ -n "${asan+x}" ];           then auto_compile_flags="${auto_compile_flags} -fsanitize=address";     echo "[asan enabled]"; fi
if [ -n "${track_arena+x}" ];    then auto_compile_flags="${auto_compile_flags} -DBUILD_TRACK_ARENA";    echo "[arena tracking enabled]"; fi
if [ -n "${metal_renderer+x}" ]; then auto_compile_flags="${auto_compile_flags} -DBUILD_METAL_RENDERER"; echo "[Metal renderer enabled]"; fi
if [ -n "${gap+x}" ] && [ -z "${no_deps+x}" ] && [ -z "${only_compile+x}" ]; then build_assets=1; fi

# --- Compile/Link Line Definitions ------------------------------------------
inc_dirs="-I../inc/
-I../src/
-I../external/nanosvg/src/
-I../external/freetype-2.13.3/include/
-I../external/stb/
-I../external/miniz/
-I../external/xxHash/
-Igen/"

defs="-DOS_MAC=1 ${auto_compile_flags}"

cc_common="${compiler} -Wall ${defs} ${inc_dirs}"
cxx_common="-std=c++20 -Wno-switch -Wno-unused-function -Wno-writable-strings -Wno-braced-scalar-init -Wno-char-subscripts"
cc_debug="${cc_common} -g -O0 -DBUILD_DEBUG=1"
cc_release="${cc_common} -g -O2 -DNDEBUG"

link_common="-lpthread -framework CoreFoundation -framework Cocoa -framework Metal -framework QuartzCore"
if [ -n "${asan+x}" ]; then link_common="${link_common} -fsanitize=address"; fi

# --- Choose Compile/Link Lines ----------------------------------------------
if [ -n "${gcc+x}" ];     then compile_debug="${cc_debug}"; fi
if [ -n "${gcc+x}" ];     then compile_release="${cc_release}"; fi
if [ -n "${clang+x}" ];   then compile_debug="${cc_debug}"; fi
if [ -n "${clang+x}" ];   then compile_release="${cc_release}"; fi
if [ -n "${debug+x}" ];   then compile="${compile_debug}"; fi
if [ -n "${release+x}" ]; then compile="${compile_release}"; fi

# --- 3rd Party Sources ------------------------------------------------------
freetype="../external/freetype-2.13.3/freetype-unity.c"
freetype_obj="freetype-unity.o"

# --- No Unity Sources -------------------------------------------------------
no_unity_src=""

# --- Unity Source -----------------------------------------------------------
gapsrc="../src/main.cpp"
gapsrc_obj="main.o"

if [ -n "${no_unity+x}" ]; then gapsrc="${no_unity_src}"; compile="${compile} -DNO_UNITY"; fi

# --- Asset Tools Source -----------------------------------------------------
asset_tool="../src/asset-tool.cpp"

# --- Scratch App Source -----------------------------------------------------
scratch_app="../scratch-app/app.cpp"
scratch_app_obj="app.o"

# --- Prep Directories -------------------------------------------------------
mkdir -p build

# --- Get Current Git Commit Id ----------------------------------------------
c_compile="${compile} -x objective-c -c -DBUILD_GIT_HASH=\"`git describe --always --dirty`\" -DBUILD_GIT_HASH_FULL=\"`git rev-parse HEAD`\""
cxx_compile="${compile} -x objective-c++ -c ${cxx_common} -DBUILD_GIT_HASH=\"`git describe --always --dirty`\" -DBUILD_GIT_HASH_FULL=\"`git rev-parse HEAD`\""
link="clang++ $freetype_obj $gapsrc_obj ${link_common} -o gap"
link_ass_tool="clang++ asset-tool.o ${link_common} -o asset-tool"
link_scratch_app="clang++ $scratch_app_obj ${link_common} -o app"

# --- Build & Run Assets -----------------------------------------------------
if [ "$build_assets" -eq 0 ]; then echo "[skipping asset build]";fi
if [ "$build_assets" -eq 1 ]
then
  pushd build
  $cxx_compile $asset_tool
  $link_ass_tool
  ./asset-tool
  popd
fi

# --- Build Everything (@build_targets) --------------------------------------
pushd build
if [ -n "${gap+x}" ]
then
  didbuild=1
  if [ -n "${only_compile+x}" ]
  then
    echo "[only compile]"
    $cxx_compile $gapsrc -ftime-report
    exit 0
  fi
  if [ -n "${no_deps+x}" ]
  then
    $cxx_compile $gapsrc
    $link
    exit 0
  fi
  $c_compile $freetype
  $cxx_compile $gapsrc
  $link
  codesign --force --deep --sign - --entitlements ../res/entitlements.plist gap
fi

if [ -n "${app+x}" ]
then
  didbuild=1
  $cxx_compile $scratch_app
  $link_scratch_app
fi
popd

# --- Warn On No Builds ------------------------------------------------------
if [ -z "${didbuild+x}" ]
then
  echo '[WARNING] no valid build target specified; must use build target names as arguments to this script, like `build gap` or `build gap release`.'
  exit 1
fi
