cmake_minimum_required(VERSION 3.28...3.30)

add_library(obs-filters MODULE)
add_library(OBS::filters ALIAS obs-filters)

if(OS_WINDOWS)
  include(cmake/onnxruntime.cmake)
endif()

include(cmake/soundtouch.cmake)

target_sources(
  obs-filters
  PRIVATE
    async-delay-filter.c
    chroma-key-filter.c
    clarity-filter.c
    color-correction-filter.c
    color-grade-filter.c
    color-key-filter.c
    compressor-filter.c
    crop-filter.c
    eq-filter.c
    expander-filter.c
    gain-filter.c
    gpu-delay.c
    hdr-tonemap-filter.c
    invert-audio-polarity.c
    limiter-filter.c
    luma-key-filter.c
    mask-filter.c
    noise-gate-filter.c
    obs-filters.c
    scale-filter.c
    scroll-filter.c
    sharpness-filter.c
    temporal-denoise-filter.c
    beauty-filter.c
    background-mask.c
    background-mask.h
    background-segmentation-filter.c
    background-blur.h
    background-blur-render.c
    background-blur-render.h
    matting-models.c
    matting-models.h
    matting-ort.c
    matting-ort.h
    audioshift-filter.c
    audioshift-soundtouch.cpp
    audioshift-soundtouch.h
)

target_link_libraries(obs-filters PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(TARGET OBS::onnxruntime)
  # FACE_SWAP_HAS_ORT is the name matting-ort.c guards on upstream, where the
  # ONNX Runtime wiring arrived with the face swap filter.  Kept verbatim so
  # future syncs from that branch do not conflict over a rename.
  target_compile_definitions(obs-filters PRIVATE FACE_SWAP_HAS_ORT=1)
  target_link_libraries(obs-filters PRIVATE OBS::onnxruntime)
  # onnxruntime.dll has to sit next to obs.exe: the loader searches the
  # executable's directory, not the plugin's.
  add_custom_command(
    TARGET obs-filters
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${ONNXRUNTIME_DLL}"
      "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
    VERBATIM
  )
endif()

if(TARGET SoundTouch::SoundTouch)
  target_compile_definitions(obs-filters PRIVATE AUDIOSHIFT_HAS_SOUNDTOUCH=1)
  target_link_libraries(obs-filters PRIVATE SoundTouch::SoundTouch)
  # No POST_BUILD DLL-copy step needed here, unlike OBS::onnxruntime above:
  # the vendored soundtouch.cmake path builds SoundTouch as an OBJECT
  # library straight into obs-filters.dll itself, not a separate shared
  # library. This also sidesteps the LGPL-2.1 dynamic-linking question
  # entirely (see docs/audioshift-design.md 3.3) -- the full corresponding
  # source is already sitting in plugins/obs-filters/soundtouch/.
endif()

include(cmake/speexdsp.cmake)
include(cmake/rnnoise.cmake)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in obs-filters.rc)
  target_sources(obs-filters PRIVATE obs-filters.rc)
endif()

set_target_properties_obs(obs-filters PROPERTIES FOLDER plugins PREFIX "")

if(ENABLE_TEST_INPUT)
  add_executable(background-mask-test background-mask.c background-mask-test.c)
  target_link_libraries(background-mask-test PRIVATE OBS::libobs)
  add_test(NAME background-mask COMMAND background-mask-test)
  set_target_properties(background-mask-test PROPERTIES FOLDER tests)

  add_executable(background-compat-test background-compat-test.c)
  add_test(NAME background-compat COMMAND background-compat-test)
  set_target_properties(background-compat-test PROPERTIES FOLDER tests)

  add_executable(background-fade-test background-fade-test.c)
  add_test(NAME background-fade COMMAND background-fade-test)
  set_target_properties(background-fade-test PROPERTIES FOLDER tests)

  add_executable(background-blur-test background-blur-test.c)
  target_compile_definitions(
    background-blur-test
    PRIVATE BLUR_EFFECT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/background_alpha.effect"
  )
  add_test(NAME background-blur COMMAND background-blur-test)
  set_target_properties(background-blur-test PROPERTIES FOLDER tests)

  if(OS_WINDOWS)
    # The blur shader drawn on a real Direct3D 11 device, headless. Not an
    # add_test: it needs a GPU and has to be run from the directory holding
    # obs.dll and libobs-d3d11.dll (see the comment at the top of the file).
    add_executable(background-blur-gpu-test background-blur-gpu-test.c background-blur-render.c)
    target_link_libraries(background-blur-gpu-test PRIVATE OBS::libobs)
    set_target_properties(background-blur-gpu-test PROPERTIES FOLDER tests)
  endif()

  add_executable(matting-models-test matting-models.c matting-models-test.c)
  target_link_libraries(matting-models-test PRIVATE OBS::libobs)
  add_test(NAME matting-models COMMAND matting-models-test)
  set_target_properties(matting-models-test PROPERTIES FOLDER tests)

  # Exercise real .onnx files through ONNX Runtime.  Both skip with success
  # when the runtime or the models are absent, so they are safe to register
  # unconditionally.
  if(TARGET OBS::onnxruntime)
    add_executable(matting-e2e-test matting-ort.c matting-models.c matting-e2e-test.c)
    target_compile_definitions(matting-e2e-test PRIVATE FACE_SWAP_HAS_ORT=1)
    target_link_libraries(matting-e2e-test PRIVATE OBS::libobs OBS::onnxruntime)
    add_test(NAME matting-e2e COMMAND matting-e2e-test)
    set_target_properties(matting-e2e-test PROPERTIES FOLDER tests)

    add_executable(matting-bench matting-ort.c matting-models.c matting-bench.c)
    target_compile_definitions(matting-bench PRIVATE FACE_SWAP_HAS_ORT=1)
    target_link_libraries(matting-bench PRIVATE OBS::libobs OBS::onnxruntime)
    set_target_properties(matting-bench PROPERTIES FOLDER tests)

    # The pinned runtime has to sit next to each executable that loads it, for
    # the same reason obs-filters copies it next to obs.exe.  Leaving it out
    # does not fail the build: the loader searches System32 before PATH, and
    # Windows ships its own older onnxruntime.dll there, so these two would
    # find an API version they cannot use and skip themselves as "ONNX Runtime
    # unavailable" - reporting success while exercising nothing.
    foreach(_ort_test matting-e2e-test matting-bench)
      add_custom_command(
        TARGET ${_ort_test}
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:${_ort_test}>"
        VERBATIM
      )
    endforeach()
  endif()

  # The functions under test are defined directly in audioshift-filter.h as
  # static inline (like util/deque.h), precisely so this test doesn't need
  # to link audioshift-filter.c itself: that file's audioshift_name() calls
  # obs_module_text(), which only exists once OBS_MODULE_USE_DEFAULT_LOCALE()
  # has run in obs-filters.c -- pulling that in would mean linking every
  # other filter's .c file too, just to satisfy the linker.
  add_executable(audioshift-filter-test audioshift-filter-test.c)
  target_link_libraries(audioshift-filter-test PRIVATE OBS::libobs)
  add_test(NAME audioshift-filter COMMAND audioshift-filter-test)
  set_target_properties(audioshift-filter-test PROPERTIES FOLDER tests)

  # Real CPU-cost measurement for the SoundTouch path; skips itself
  # (see audioshift-bench.c) when no real SoundTouch was built.
  if(TARGET SoundTouch::SoundTouch)
    add_executable(audioshift-bench audioshift-soundtouch.cpp audioshift-bench.c)
    target_compile_definitions(audioshift-bench PRIVATE AUDIOSHIFT_HAS_SOUNDTOUCH=1)
    target_link_libraries(audioshift-bench PRIVATE OBS::libobs SoundTouch::SoundTouch)
    set_target_properties(audioshift-bench PROPERTIES FOLDER tests)
  endif()
endif()
