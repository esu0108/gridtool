cmake_minimum_required(VERSION 3.24)
project(ImageGridTool LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows (DirectX 12) only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Dear ImGui (Win32 + DX12 backends).
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.93.0
    GIT_SHALLOW    TRUE
)

# stb_image.h. stb has no release tags; pin a commit hash here if you need reproducibility.
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# stb_image_resize2.h now lives in its own repository (it was removed from the stb repo's top level).
FetchContent_Declare(
    stb_resize
    GIT_REPOSITORY https://github.com/nothings/stb_image_resize.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(imgui stb stb_resize)

add_executable(ImageGridTool WIN32
    src/main.cpp
    src/App.cpp
    src/D3D12Context.cpp
    src/ImageLoader.cpp
    src/Dialogs.cpp
    src/BmpWriter.cpp
    src/stb_impl.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)

target_include_directories(ImageGridTool PRIVATE
    src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${stb_SOURCE_DIR}
    ${stb_resize_SOURCE_DIR}
)

target_compile_definitions(ImageGridTool PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(ImageGridTool PRIVATE /utf-8 /W4 /permissive-)
endif()

target_link_libraries(ImageGridTool PRIVATE
    d3d12 dxgi ole32 uuid shell32
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ImageGridTool)
