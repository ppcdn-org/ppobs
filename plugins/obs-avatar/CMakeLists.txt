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
)

configure_file(cmake/windows/obs-module.rc.in obs-avatar.rc)
target_sources(obs-avatar PRIVATE obs-avatar.rc)

target_link_libraries(obs-avatar PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

set_target_properties_obs(obs-avatar PROPERTIES FOLDER plugins PREFIX "")

if(ENABLE_TEST_INPUT)
  # The ring buffer and level maths are static inline in avatar-audio-tap.h
  # precisely so this test does not have to link avatar-audio-tap.c, which
  # calls obs_* and would pull the whole module in.  Same arrangement as
  # obs-filters' audioshift-filter-test.
  add_executable(avatar-audio-tap-test avatar-audio-tap-test.c)
  target_link_libraries(avatar-audio-tap-test PRIVATE OBS::libobs)
  add_test(NAME avatar-audio-tap COMMAND avatar-audio-tap-test)
  set_target_properties(avatar-audio-tap-test PROPERTIES FOLDER tests)
endif()
