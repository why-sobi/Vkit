cmake_minimum_required(VERSION 3.20)
project(encoder CXX)

set(CMAKE_CXX_STANDARD 23)

add_library(encoder
    src/encoder.cpp
)

target_include_directories(encoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# VKIT_ENCODER_BACKEND ("INTEL" or "NVIDIA") is set by the top-level CMakeLists.
# The backend definitions are PUBLIC because encoder.hpp picks the JpegEncoder
# alias from them, so every consumer must see the same one.
if (VKIT_ENCODER_BACKEND STREQUAL "INTEL")
    message(STATUS "Configuring Encoder with Intel oneVPL Backend")

    # Force oneVPL to build statically
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/libvpl)

    target_sources(encoder PRIVATE src/encoder_intel.cpp)

    # "VPL" is the libvpl dispatcher target (it outputs vpl.lib / vpld.lib) and
    # brings the oneVPL headers with it. The name is case-sensitive.
    target_link_libraries(encoder PUBLIC
        VPL
        d3d11
        dxgi
    )
    target_compile_definitions(encoder PUBLIC VKIT_USE_INTEL_VPL)

elseif (VKIT_ENCODER_BACKEND STREQUAL "NVIDIA")
    message(STATUS "Configuring Encoder with NVIDIA nvJPEG Backend")

    find_package(CUDAToolkit REQUIRED)
    enable_language(CUDA)

    # encoder_nvidia.cpp owns the nvJPEG orchestration; encoder_cuda.cu contains
    # the CUDA kernel used for D3D11-texture color conversion.
    target_sources(encoder PRIVATE
        src/encoder_nvidia.cpp
        src/encoder_cuda.cu
    )
    target_compile_definitions(encoder PUBLIC VKIT_USE_NVIDIA_NVJPEG)

    target_link_libraries(encoder PUBLIC
        CUDA::cudart
        CUDA::nvjpeg
        d3d11
        dxgi
    )
    set_target_properties(encoder PROPERTIES
        CUDA_STANDARD 17
        CUDA_ARCHITECTURES 86
    )

else()
    message(FATAL_ERROR "Unknown VKIT_ENCODER_BACKEND '${VKIT_ENCODER_BACKEND}'. Use INTEL or NVIDIA.")
endif()

# Optional test executable
# add_executable(encoder_test tests/jpeg_encoder_test.cpp)
# target_link_libraries(encoder_test PRIVATE encoder)
# if (WIN32)
#     target_link_libraries(encoder_test PRIVATE shell32)
# endif()
