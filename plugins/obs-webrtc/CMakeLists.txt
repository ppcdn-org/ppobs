cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_WEBRTC "Enable WebRTC Output support" ON)
if(NOT ENABLE_WEBRTC)
  target_disable(obs-webrtc)
  return()
endif()

find_package(LibDataChannel 0.20 REQUIRED)
find_package(CURL REQUIRED)
find_package(Websocketpp 0.8 REQUIRED)
find_package(Asio 1.12.1 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
# avcodec/swscale: quality-score.cpp decodes the stream's own top layer
# back and compares it against the program feed.
find_package(FFmpeg REQUIRED avcodec avutil swscale)

add_library(obs-webrtc MODULE)
add_library(OBS::webrtc ALIAS obs-webrtc)

target_sources(
  obs-webrtc
  PRIVATE obs-webrtc.cpp ppcenter-client.cpp ppcenter-client.h ppcenter-signal-message.cpp ppcenter-signal-message.h ppcenter-signal-outgoing.cpp ppcenter-signal-outgoing.h ppcenter-websocket-frame.cpp ppcenter-websocket-frame.h ppcenter-websocket-utils.cpp ppcenter-websocket-utils.h ppcenter-signal.cpp ppcenter-signal.h uplink-qos-policy.cpp uplink-qos-policy.h whip-output.cpp whip-output.h whip-codec-session.cpp whip-codec-session.h whip-service.cpp whip-service.h whip-utils.h degrade-client.cpp degrade-client.h quality-score.cpp quality-score.h
)

target_compile_definitions(
  obs-webrtc
  PRIVATE WHIP_DEGRADE_ACTIVE ASIO_STANDALONE $<$<PLATFORM_ID:Windows>:_WEBSOCKETPP_CPP11_STL_>
          $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0603>
)

target_link_libraries(
  obs-webrtc
  PRIVATE OBS::libobs
          LibDataChannel::LibDataChannel
          CURL::libcurl
          Websocketpp::Websocketpp
          Asio::Asio
          OpenSSL::SSL
          OpenSSL::Crypto
          FFmpeg::avcodec
          FFmpeg::avutil
          FFmpeg::swscale
)

set_target_properties_obs(obs-webrtc PROPERTIES FOLDER plugins PREFIX "")

add_executable(obs-webrtc-p2p-signal-message-test EXCLUDE_FROM_ALL tests/p2p-signal-message-test.cpp ppcenter-signal-message.cpp ppcenter-signal-message.h)
target_include_directories(obs-webrtc-p2p-signal-message-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obs-webrtc-p2p-signal-message-test PRIVATE OBS::libobs)

add_executable(obs-webrtc-p2p-websocket-utils-test EXCLUDE_FROM_ALL tests/p2p-websocket-utils-test.cpp ppcenter-websocket-utils.cpp ppcenter-websocket-utils.h)
target_include_directories(obs-webrtc-p2p-websocket-utils-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(obs-webrtc-p2p-websocket-frame-test EXCLUDE_FROM_ALL tests/p2p-websocket-frame-test.cpp ppcenter-websocket-frame.cpp ppcenter-websocket-frame.h)
target_include_directories(obs-webrtc-p2p-websocket-frame-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(obs-webrtc-p2p-signal-outgoing-test EXCLUDE_FROM_ALL tests/p2p-signal-outgoing-test.cpp ppcenter-signal-outgoing.cpp ppcenter-signal-outgoing.h ppcenter-signal-message.cpp ppcenter-signal-message.h)
target_include_directories(obs-webrtc-p2p-signal-outgoing-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obs-webrtc-p2p-signal-outgoing-test PRIVATE OBS::libobs)

add_executable(obs-webrtc-uplink-qos-policy-test EXCLUDE_FROM_ALL tests/uplink-qos-policy-test.cpp uplink-qos-policy.cpp uplink-qos-policy.h)
target_include_directories(obs-webrtc-uplink-qos-policy-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
