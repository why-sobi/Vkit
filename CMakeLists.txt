cmake_minimum_required(VERSION 3.20)

project(VKIT VERSION 1.0.0 LANGUAGES CXX)

# Global C++ standard settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(
    CANVAS_VRAM_RESIDENCE
    CANVAS_MOUSE_DISPLAY
)

# Add submodules (e.g. canvas, audio, input, etc.)
add_subdirectory(canvas)
add_subdirectory(scaler)
add_subdirectory(encoder)


# Top-level executable combining all modules
add_executable(VKIT
    src/main.cpp
)

# Link internal submodules to the main application
target_link_libraries(VKIT
    PRIVATE
        canvas
        scaler
        encoder
)
