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

find_package(CUDAToolkit QUIET)
if (CUDAToolkit_FOUND AND TARGET CUDA::nvjpeg)
    enable_language(CUDA)
    target_compile_definitions(encoder PRIVATE VKIT_HAS_NVJPEG)
    # encoder.cpp owns the C++/nvJPEG orchestration; encoder_cuda.cu contains
    # the CUDA kernel used for D3D11-texture color conversion.
    target_sources(encoder PRIVATE src/encoder_cuda.cu)
    target_link_libraries(encoder PUBLIC CUDA::cudart CUDA::nvjpeg)
    set_target_properties(encoder PROPERTIES
        CUDA_STANDARD 17
        CUDA_ARCHITECTURES 86
    )

    add_executable(jpeg_encoder_test
        tests/jpeg_encoder_test.cpp
    )
    target_link_libraries(jpeg_encoder_test PRIVATE encoder)
    if (WIN32)
        target_link_libraries(jpeg_encoder_test PRIVATE shell32)
    endif()
else()
    message(WARNING "CUDA Toolkit/nvJPEG not found: nvJPEG is disabled for this build.")
endif()