# SoundTouch (LGPL-2.1, Olli Parviainen) backs the AudioShift pitch-shifting
# filter. See docs/audioshift-design.md secs 3.3/12 and
# plugins/obs-filters/soundtouch/VENDORED-FROM.md for provenance: the
# "SoundTouch" subdirectory here vendors the library sources (not the CLI
# tool, DLL wrapper, or upstream's own build files) straight from the
# upstream tag, the same way rnnoise.cmake vendors RNNoise.

set(SOUNDTOUCH_VERSION "2.4.1" CACHE STRING "SoundTouch version")

# 1) A CMake package, if one happens to already be installed/exported
#    (SoundTouch's own CMakeLists.txt exports "SoundTouch::SoundTouch").
#    Optional and quiet: most machines won't have it, and the vendored
#    fallback below covers them.
find_package(SoundTouch ${SOUNDTOUCH_VERSION} CONFIG QUIET)

if(NOT TARGET SoundTouch::SoundTouch)
  add_library(obs-soundtouch OBJECT)

  target_sources(
    obs-soundtouch
    PRIVATE
      soundtouch/source/SoundTouch/AAFilter.cpp
      soundtouch/source/SoundTouch/AAFilter.h
      soundtouch/source/SoundTouch/cpu_detect.h
      soundtouch/source/SoundTouch/cpu_detect_x86.cpp
      soundtouch/source/SoundTouch/FIFOSampleBuffer.cpp
      soundtouch/source/SoundTouch/FIRFilter.cpp
      soundtouch/source/SoundTouch/FIRFilter.h
      soundtouch/source/SoundTouch/InterpolateCubic.cpp
      soundtouch/source/SoundTouch/InterpolateCubic.h
      soundtouch/source/SoundTouch/InterpolateLinear.cpp
      soundtouch/source/SoundTouch/InterpolateLinear.h
      soundtouch/source/SoundTouch/InterpolateShannon.cpp
      soundtouch/source/SoundTouch/InterpolateShannon.h
      soundtouch/source/SoundTouch/mmx_optimized.cpp
      soundtouch/source/SoundTouch/PeakFinder.cpp
      soundtouch/source/SoundTouch/PeakFinder.h
      soundtouch/source/SoundTouch/RateTransposer.cpp
      soundtouch/source/SoundTouch/RateTransposer.h
      soundtouch/source/SoundTouch/SoundTouch.cpp
      soundtouch/source/SoundTouch/sse_optimized.cpp
      soundtouch/source/SoundTouch/TDStretch.cpp
      soundtouch/source/SoundTouch/TDStretch.h
    PUBLIC
      soundtouch/include/FIFOSampleBuffer.h
      soundtouch/include/FIFOSamplePipe.h
      soundtouch/include/soundtouch_config.h
      soundtouch/include/SoundTouch.h
      soundtouch/include/STTypes.h
  )

  target_include_directories(obs-soundtouch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/soundtouch/include")

  # Float samples to match OBS's AUDIO_FORMAT_FLOAT_PLANAR mix format (see
  # docs/audioshift-design.md sec 4.3); PUBLIC so audioshift-soundtouch.cpp
  # (which includes SoundTouch.h too) agrees on SAMPLETYPE==float.
  target_compile_definitions(obs-soundtouch PUBLIC SOUNDTOUCH_FLOAT_SAMPLES)

  set_target_properties(obs-soundtouch PROPERTIES FOLDER plugins/obs-filters/soundtouch POSITION_INDEPENDENT_CODE TRUE)

  add_library(SoundTouch::SoundTouch ALIAS obs-soundtouch)
endif()
