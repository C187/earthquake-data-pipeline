cmake_minimum_required(VERSION 3.12)
project(earthquake_data_pipeline VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)

add_executable(earthquake_pipeline
    src/main.cpp
)

target_include_directories(earthquake_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(earthquake_pipeline PRIVATE CURL::libcurl)

