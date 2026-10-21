cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_FFMPEG_LOGGING "Enables obs-ffmpeg logging" OFF)
option(ENABLE_NEW_MPEGTS_OUTPUT "Use native SRT/RIST mpegts output" ON)

if(OS_LINUX OR OS_WINDOWS)
  option(ENABLE_FFMPEG_NVENC "Enable legacy FFmpeg NVENC encoder" OFF)
endif()

include(cmake/dependencies.cmake)

if(ENABLE_NEW_MPEGTS_OUTPUT)
  find_package(Websocketpp 0.8 REQUIRED)
  find_package(Asio 1.12.1 REQUIRED)
  find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
endif()

add_library(obs-ffmpeg MODULE)
add_library(OBS::ffmpeg ALIAS obs-ffmpeg)

add_subdirectory(ffmpeg-mux)

target_sources(
  obs-ffmpeg
  PRIVATE
    $<$<BOOL:${ENABLE_FFMPEG_LOGGING}>:obs-ffmpeg-logging.c>
    $<$<BOOL:${ENABLE_FFMPEG_NVENC}>:obs-ffmpeg-nvenc.c>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-mpegts.c>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-rist.h>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-srt.h>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-url.h>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:srt-degrade-client.cpp>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:srt-degrade-client.h>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:obs-ffmpeg-vaapi.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-utils.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-utils.h>
    obs-ffmpeg-audio-encoders.c
    obs-ffmpeg-av1.c
    obs-ffmpeg-compat.h
    obs-ffmpeg-ertsp.c
    obs-ffmpeg-formats.h
    obs-ffmpeg-openh264.c
    obs-ffmpeg-hls-mux.c
    obs-ffmpeg-mux.c
    obs-ffmpeg-mux.h
    obs-ffmpeg-output.c
    obs-ffmpeg-output.h
    obs-ffmpeg-source.c
    obs-ffmpeg-video-encoders.c
    obs-ffmpeg.c
)

target_include_directories(obs-ffmpeg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")

target_compile_options(obs-ffmpeg PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-shorten-64-to-32>)
target_compile_definitions(
  obs-ffmpeg
  PRIVATE
    $<$<BOOL:${ENABLE_FFMPEG_LOGGING}>:ENABLE_FFMPEG_LOGGING>
    $<$<BOOL:${ENABLE_FFMPEG_NVENC}>:ENABLE_FFMPEG_NVENC>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:NEW_MPEGTS_OUTPUT>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:ASIO_STANDALONE>
    $<$<AND:$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>,$<PLATFORM_ID:Windows>>:_WEBSOCKETPP_CPP11_STL_>
    $<$<AND:$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>,$<PLATFORM_ID:Windows>>:_WIN32_WINNT=0x0603>
)

target_link_libraries(
  obs-ffmpeg
  PRIVATE
    OBS::libobs
    OBS::media-playback
    OBS::opts-parser
    FFmpeg::avcodec
    FFmpeg::avfilter
    FFmpeg::avformat
    FFmpeg::avdevice
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    OBS::frontend-api
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libva::va>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libva::drm>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libpci::pci>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libdrm::Libdrm>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:Librist::Librist>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:Libsrt::Libsrt>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:Websocketpp::Websocketpp>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:Asio::Asio>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:OpenSSL::SSL>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:OpenSSL::Crypto>
    $<$<AND:$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>,$<PLATFORM_ID:Windows>>:iphlpapi>
)

if(OS_WINDOWS)
  if(CMAKE_VS_PLATFORM_NAME STREQUAL x64)
    target_sources(obs-ffmpeg PRIVATE texture-amf-opts.hpp texture-amf.cpp)
    target_link_libraries(obs-ffmpeg PRIVATE AMF::AMF)
  endif()

  configure_file(cmake/windows/obs-module.rc.in obs-ffmpeg.rc)
  target_sources(obs-ffmpeg PRIVATE obs-ffmpeg.rc)
endif()

set_target_properties_obs(obs-ffmpeg PROPERTIES FOLDER plugins/obs-ffmpeg PREFIX "")

if(ENABLE_TEST_INPUT)
  # publish-url-redact.h is header-only, so this needs nothing of the plugin
  # itself. It links libobs only for dstr.
  add_executable(publish-url-redact-test publish-url-redact-test.c)
  target_include_directories(publish-url-redact-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
  target_link_libraries(publish-url-redact-test PRIVATE OBS::libobs)
  add_test(NAME publish-url-redact COMMAND publish-url-redact-test)
  set_target_properties(publish-url-redact-test PROPERTIES FOLDER tests)
endif()
