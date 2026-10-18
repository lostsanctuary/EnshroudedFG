cmake_minimum_required(VERSION 3.24)
project(EnshroudedFG LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EnshroudedFG must be built for x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the proxy has no VC++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

foreach(dep external/minhook/include/MinHook.h external/imgui/imgui.h)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dep})
        message(FATAL_ERROR "${dep} missing - run: git submodule update --init")
    endif()
endforeach()
# NVIDIA's NGX headers are not redistributable in source form; tools/fetch_deps.ps1 downloads them.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/ngx/include/nvsdk_ngx_vk.h)
    message(FATAL_ERROR "NGX headers missing - run: powershell -ExecutionPolicy Bypass -File tools/fetch_deps.ps1")
endif()

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
efg_shader(copy_rgba8 kCopyRgba8Spv)
efg_shader(copy_r32 kCopyR32Spv)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_NO_PROTOTYPES
                                        IMGUI_IMPL_WIN32_DISABLE_GAMEPAD IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC Vulkan::Headers)

add_library(EnshroudedFG SHARED
    src/dllmain.cpp
    src/camera/camera_provider.cpp
    src/core/config.cpp
    src/core/log.cpp
    src/core/module_ranges.cpp
    src/sl/sl_bridge.cpp
    src/render/copy_pass.cpp
    src/overlay/overlay.cpp
    src/overlay/theme.cpp
    src/debug/frame_trace.cpp
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
target_link_libraries(EnshroudedFG PRIVATE minhook imgui Vulkan::Headers wintrust crypt32)
