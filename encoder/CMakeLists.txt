cmake_minimum_required(VERSION 3.20)
project(encoder CXX)

set(CMAKE_CXX_STANDARD 23)

add_library(encoder
    src/encoder.cpp
    src/encoder_intel.cpp
)

target_include_directories(encoder PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Automatically link DirectX dependencies so consuming projects don't have to
target_link_libraries(encoder PUBLIC 
    d3d11 
    dxgi
)