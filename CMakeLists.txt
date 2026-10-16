cmake_minimum_required(VERSION 4.2)
project(map_tile_viewer C)

set(CMAKE_C_STANDARD 99)

find_package(raylib CONFIG REQUIRED)
find_package(CURL REQUIRED)

add_executable(map_tile_viewer main.c)

target_compile_definitions(map_tile_viewer PRIVATE CURL_STATICLIB)
target_link_libraries(map_tile_viewer PRIVATE raylib CURL::libcurl)
