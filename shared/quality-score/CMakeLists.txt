cmake_minimum_required(VERSION 3.28...3.30)

# avcodec/swscale: quality-score.cpp decodes the output's own top layer
# back and compares it against the program feed.
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale)

add_library(quality-score OBJECT)
add_library(OBS::quality-score ALIAS quality-score)

target_sources(quality-score PRIVATE quality-score.cpp PUBLIC quality-score.h)

target_include_directories(quality-score PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(quality-score PUBLIC OBS::libobs FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale)

set_target_properties(quality-score PROPERTIES FOLDER deps POSITION_INDEPENDENT_CODE TRUE)
