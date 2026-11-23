cmake_minimum_required(VERSION 3.20)
project(LiquidGlassTaskbar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(LiquidGlassTaskbar WIN32 main.cpp)

target_compile_definitions(LiquidGlassTaskbar PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    set_property(TARGET LiquidGlassTaskbar PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(LiquidGlassTaskbar PRIVATE /O2 /W3 /EHsc /utf-8)
endif()

target_link_libraries(LiquidGlassTaskbar PRIVATE
    d3d11 dxgi d3dcompiler dcomp dwmapi
    user32 shell32 gdi32 shcore ole32 shlwapi)

set_target_properties(LiquidGlassTaskbar PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
