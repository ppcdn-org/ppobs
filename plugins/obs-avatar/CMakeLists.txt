cmake_minimum_required(VERSION 3.28...3.30)

# Windows-only for now, like nv-filters: stage C's MuseTalk worker is a Windows
# process and nothing here has been exercised on the other platforms.
if(NOT OS_WINDOWS)
  return()
endif()

add_library(obs-avatar MODULE)
add_library(OBS::avatar ALIAS obs-avatar)

target_sources(
  obs-avatar
  PRIVATE
    obs-avatar.c
    avatar-source.c
    avatar-source.h
    avatar-audio-tap.c
    avatar-audio-tap.h
    avatar-assets.c
    avatar-assets.h
    avatar-engine.h
    avatar-engine-sidecar.c
    avatar-engine-sidecar.h
    avatar-blink.c
    avatar-blink.h
    avatar-eyes.h
    avatar-engine-viseme.c
    avatar-engine-viseme.h
    avatar-sidecar-protocol.h
)

configure_file(cmake/windows/obs-module.rc.in obs-avatar.rc)
target_sources(obs-avatar PRIVATE obs-avatar.rc)

target_link_libraries(obs-avatar PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

set_target_properties_obs(obs-avatar PROPERTIES FOLDER plugins PREFIX "")

# The worker the source launches when its own "worker script" setting is empty.
# It stays in the source tree rather than the installer - see
# tools/musetalk-sidecar/README.md - so a build made elsewhere points at a path
# that does not exist there, which the engine reports instead of guessing.
target_compile_definitions(
  obs-avatar
  PRIVATE AVATAR_DEFAULT_WORKER_SCRIPT="${CMAKE_SOURCE_DIR}/tools/musetalk-sidecar/ppobs_avatar_worker.py"
)

if(ENABLE_TEST_INPUT)
  # The ring buffer and level maths are static inline in avatar-audio-tap.h
  # precisely so this test does not have to link avatar-audio-tap.c, which
  # calls obs_* and would pull the whole module in.  Same arrangement as
  # obs-filters' audioshift-filter-test.
  add_executable(avatar-audio-tap-test avatar-audio-tap-test.c)
  target_link_libraries(avatar-audio-tap-test PRIVATE OBS::libobs)
  add_test(NAME avatar-audio-tap COMMAND avatar-audio-tap-test)
  set_target_properties(avatar-audio-tap-test PROPERTIES FOLDER tests)

  # Header-only, so this one needs nothing linked at all. The byte fixtures it
  # pins are asserted again by the worker's --selftest from Python: the two
  # ends of this protocol are written in different languages, so disagreeing
  # on the wire format is the failure it is most likely to suffer.
  add_executable(avatar-protocol-test avatar-protocol-test.c)
  add_test(NAME avatar-protocol COMMAND avatar-protocol-test)
  set_target_properties(avatar-protocol-test PROPERTIES FOLDER tests)

  # Mode B's openness mapping and smoothing, which are static inline in
  # avatar-engine-viseme.h so this needs none of the engine's graphics code.
  add_executable(avatar-viseme-test avatar-viseme-test.c)
  target_link_libraries(avatar-viseme-test PRIVATE OBS::libobs)
  add_test(NAME avatar-viseme COMMAND avatar-viseme-test)
  set_target_properties(avatar-viseme-test PROPERTIES FOLDER tests)

  # The one test that makes the two ends actually talk: it compiles the real
  # client in and starts the real worker as a child process. Fixtures prove
  # the two agree on bytes; only this proves they agree on a conversation.
  #
  # It needs Python at run time, which not every machine building OBS has, so
  # it skips loudly rather than failing when the interpreter is missing.
  add_executable(avatar-sidecar-loopback-test avatar-sidecar-loopback-test.c avatar-engine-sidecar.c)
  target_link_libraries(avatar-sidecar-loopback-test PRIVATE OBS::libobs OBS::w32-pthreads)
  target_compile_definitions(
    avatar-sidecar-loopback-test
    PRIVATE AVATAR_WORKER_SCRIPT="${CMAKE_SOURCE_DIR}/tools/musetalk-sidecar/ppobs_avatar_worker.py"
  )
  add_test(NAME avatar-sidecar-loopback COMMAND avatar-sidecar-loopback-test)
  set_target_properties(avatar-sidecar-loopback-test PROPERTIES FOLDER tests)

  # The engine starting, watching and relaunching the worker itself, which the
  # loopback test sidesteps by starting the worker by hand. Same needs, same
  # skip when there is no interpreter.
  add_executable(avatar-sidecar-launch-test avatar-sidecar-launch-test.c avatar-engine-sidecar.c)
  target_link_libraries(avatar-sidecar-launch-test PRIVATE OBS::libobs OBS::w32-pthreads)
  target_compile_definitions(
    avatar-sidecar-launch-test
    PRIVATE AVATAR_WORKER_SCRIPT="${CMAKE_SOURCE_DIR}/tools/musetalk-sidecar/ppobs_avatar_worker.py"
  )
  add_test(NAME avatar-sidecar-launch COMMAND avatar-sidecar-launch-test)
  set_target_properties(avatar-sidecar-launch-test PROPERTIES FOLDER tests)

  # The blink shader drawn on a real Direct3D 11 device, headless, to BMP files
  # that can be compared with the picture it was designed against. Not an
  # add_test: it needs a GPU and has to be run from the directory holding
  # obs.dll and libobs-d3d11.dll (see the comment at the top of the file).
  add_executable(avatar-blink-test avatar-blink-test.c avatar-blink.c)
  target_link_libraries(avatar-blink-test PRIVATE OBS::libobs)
  set_target_properties(avatar-blink-test PROPERTIES FOLDER tests)

  # When it blinks and how long it takes, which needs no GPU.
  add_executable(avatar-blink-timing-test avatar-blink-timing-test.c avatar-blink.c)
  target_link_libraries(avatar-blink-timing-test PRIVATE OBS::libobs)
  add_test(NAME avatar-blink-timing COMMAND avatar-blink-timing-test)
  set_target_properties(avatar-blink-timing-test PROPERTIES FOLDER tests)
endif()
