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
find_package(Vulkan REQUIRED COMPONENTS glslc)  # headers + shader compiler; vulkan-1 is resolved at runtime
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Compute shaders -> SPIR-V -> embedded C++ arrays
set(EFG_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${EFG_GEN_DIR}/render)
function(efg_shader name array)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${name}.comp)
    set(spv ${EFG_GEN_DIR}/${name}.spv)
    set(hdr ${EFG_GEN_DIR}/render/${name}_spv.h)
    add_custom_command(OUTPUT ${hdr}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.2 -O -o ${spv} ${src}
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py ${spv} ${hdr} ${array}
        DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py
        COMMENT "Compiling shader ${name}.comp")
    set(EFG_SHADER_HEADERS ${EFG_SHADER_HEADERS} ${hdr} PARENT_SCOPE)
endfunction()
efg_shader(mv_convert kMvConvertSpv)

add_library(EnshroudedFG SHARED
    src/dllmain.cpp
    src/core/config.cpp
    src/core/log.cpp
    src/core/module_ranges.cpp
    src/sl/sl_bridge.cpp
    src/render/mv_convert.cpp
    ${EFG_SHADER_HEADERS}
    src/hooks/loader_hooks.cpp
    src/hooks/vulkan_hooks.cpp
    src/hooks/ngx_hooks.cpp
    src/proxy/winmm_proxy.cpp
    src/proxy/generated/winmm_stubs.asm
    src/proxy/generated/winmm.def
)
set_target_properties(EnshroudedFG PROPERTIES OUTPUT_NAME winmm)
target_include_directories(EnshroudedFG PRIVATE src ${EFG_GEN_DIR} external/ngx/include external/streamline/include)
target_compile_definitions(EnshroudedFG PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(EnshroudedFG PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_link_libraries(EnshroudedFG PRIVATE minhook Vulkan::Headers wintrust crypt32)
