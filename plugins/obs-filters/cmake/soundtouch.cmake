# SoundTouch (LGPL-2.1, Olli Parviainen) backs the AudioShift pitch-shifting
# filter. See docs/audioshift-design.md secs 3.3/12: this repository has not
# been given permission to download/vendor the real source yet, so unlike
# rnnoise.cmake there is no in-tree fallback here -- only detection of an
# already-available build, with a graceful no-op (AudioShift passes audio
# through unchanged) when none is found.

set(SOUNDTOUCH_VERSION "2.4.1" CACHE STRING "SoundTouch version")
set(SOUNDTOUCH_ROOT "" CACHE PATH "Root of a built SoundTouch tree (contains include/SoundTouch.h and a built library)")

# 1) A CMake package, if one happens to already be installed/exported.
#    SoundTouch ships its own SoundTouchConfig.cmake.in on the project's
#    current home (codeberg.org/soundtouch/soundtouch).
find_package(SoundTouch CONFIG QUIET)

# 2) The conventional .deps location, mirroring onnxruntime.cmake, if no
#    SOUNDTOUCH_ROOT was given explicitly and no package was found.
if(NOT TARGET SoundTouch::SoundTouch AND NOT SOUNDTOUCH_ROOT)
  set(_soundtouch_default "${CMAKE_SOURCE_DIR}/.deps/soundtouch-${SOUNDTOUCH_VERSION}")
  if(EXISTS "${_soundtouch_default}/include/SoundTouch.h")
    set(SOUNDTOUCH_ROOT "${_soundtouch_default}")
    message(STATUS "Found SoundTouch ${SOUNDTOUCH_VERSION} in .deps")
  endif()
  unset(_soundtouch_default)
endif()

if(NOT TARGET SoundTouch::SoundTouch AND NOT SOUNDTOUCH_ROOT)
  message(
    STATUS
    "SoundTouch not configured; the AudioShift filter will build and "
    "register but will passthrough audio unchanged (see "
    "docs/audioshift-design.md secs 3.3/12). Unpack a built SoundTouch "
    "${SOUNDTOUCH_VERSION} tree to .deps/soundtouch-${SOUNDTOUCH_VERSION}, "
    "or set SOUNDTOUCH_ROOT."
  )
  return()
endif()

# A plain INTERFACE library rather than an IMPORTED one, same rationale as
# OBS::onnxruntime in onnxruntime.cmake: OBS walks the link dependencies of
# every target while bundling the frontend, and that walk can't see IMPORTED
# targets declared outside this directory.
add_library(obs-soundtouch INTERFACE)
add_library(OBS::soundtouch ALIAS obs-soundtouch)

if(TARGET SoundTouch::SoundTouch)
  target_link_libraries(obs-soundtouch INTERFACE SoundTouch::SoundTouch)
else()
  # SOUNDTOUCH_ROOT path: expected to point at an already-built tree (this
  # project does not vendor SoundTouch's own build system the way
  # rnnoise.cmake vendors RNNoise's sources, because the real source has not
  # been fetched into this repository yet). The library name/location below
  # is a best guess to verify once the dependency is actually introduced.
  set(SOUNDTOUCH_INCLUDE_DIR "${SOUNDTOUCH_ROOT}/include")
  find_library(SOUNDTOUCH_LIBRARY NAMES SoundTouch soundtouch PATHS "${SOUNDTOUCH_ROOT}/lib" NO_DEFAULT_PATH)

  if(NOT EXISTS "${SOUNDTOUCH_INCLUDE_DIR}/SoundTouch.h" OR NOT SOUNDTOUCH_LIBRARY)
    message(FATAL_ERROR "SOUNDTOUCH_ROOT does not contain a built SoundTouch ${SOUNDTOUCH_VERSION} tree")
  endif()

  target_include_directories(obs-soundtouch INTERFACE "${SOUNDTOUCH_INCLUDE_DIR}")
  target_link_libraries(obs-soundtouch INTERFACE "${SOUNDTOUCH_LIBRARY}")
endif()
