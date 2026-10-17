cmake_minimum_required(VERSION 3.24)
project(EnshroudedFG LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EnshroudedFG must be built for x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the proxy has no VC++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(external/minhook EXCLUDE_FROM_ALL)

add_library(EnshroudedFG SHARED
    src/dllmain.cpp
    src/core/log.cpp
    src/proxy/winmm_proxy.cpp
    src/proxy/generated/winmm_stubs.asm
    src/proxy/generated/winmm.def
)
set_target_properties(EnshroudedFG PROPERTIES OUTPUT_NAME winmm)
target_include_directories(EnshroudedFG PRIVATE src)
target_compile_definitions(EnshroudedFG PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(EnshroudedFG PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_link_libraries(EnshroudedFG PRIVATE minhook)
