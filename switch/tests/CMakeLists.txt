# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests of the Switch renderer on desktop Linux with Mesa (llvmpipe
# works): the GLSL shader path and the OpenGL backend behind the presenter API.
#
#   cmake -S switch/tests -B build/switch-tests -G Ninja && cmake --build build/switch-tests
#   build/switch-tests/shader_glsl_test && build/switch-tests/gl_backend_test
#   build/switch-tests/capture_trust_test
#   RECOMP_D3D_SCALE=1.5 build/switch-tests/gl_backend_test
cmake_minimum_required(VERSION 3.20)
project(doa2u_nx_host_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
foreach(_backend HLSL MSL CPP REFLECT C_API UTIL)
    set(SPIRV_CROSS_ENABLE_${_backend} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG f0bd0257c308b9a26562c1a30c4748a0219cc951)  # 16.2.0, as the Switch build
FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG a0fba56c34a6700f1724bf9b751da5b488a3775c)  # vulkan-sdk-1.4.341.0
FetchContent_Declare(egl_registry GIT_REPOSITORY https://github.com/KhronosGroup/EGL-Registry.git
    GIT_TAG main GIT_SHALLOW TRUE SOURCE_SUBDIR none)
FetchContent_Declare(gl_registry GIT_REPOSITORY https://github.com/KhronosGroup/OpenGL-Registry.git
    GIT_TAG main GIT_SHALLOW TRUE SOURCE_SUBDIR none)
FetchContent_MakeAvailable(glslang spirv_cross egl_registry gl_registry)
find_library(EGL_LIBRARY NAMES EGL libEGL.so.1 REQUIRED)
find_library(GL_LIBRARY NAMES GL OpenGL libGL.so.1 REQUIRED)

set(RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp-runtime")
set(NX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_library(shader_path STATIC
    "${RUNTIME}/d3d_shader_compiler.cpp" "${RUNTIME}/d3d_draw_shader.cpp" "${RUNTIME}/d3d_vertex_program.cpp"
    "${RUNTIME}/d3d_draw_model.c" "${RUNTIME}/d3d_texture_model.c")
target_compile_definitions(shader_path PUBLIC RECOMP_SHADER_GLSLANG)
target_include_directories(shader_path PUBLIC "${RUNTIME}")
target_link_libraries(shader_path PUBLIC glslang glslang-default-resource-limits SPIRV spirv-cross-glsl)
# The installed include layout (spirv_cross/*.hpp) the compiler expects.
file(GLOB _spvc_headers "${spirv_cross_SOURCE_DIR}/*.h" "${spirv_cross_SOURCE_DIR}/*.hpp")
file(COPY ${_spvc_headers} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/include/spirv_cross")
file(GLOB _glslang_spirv_headers "${glslang_SOURCE_DIR}/SPIRV/*.h" "${glslang_SOURCE_DIR}/SPIRV/*.hpp")
file(COPY ${_glslang_spirv_headers} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/include/glslang/SPIRV")
target_include_directories(shader_path PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/include" "${glslang_SOURCE_DIR}")

add_executable(shader_glsl_test shader_glsl_test.cpp)
target_link_libraries(shader_glsl_test PRIVATE shader_path)
add_executable(shader_program_fuzz shader_program_fuzz.cpp)
target_link_libraries(shader_program_fuzz PRIVATE shader_path)

add_executable(gl_backend_test gl_backend_test.cpp "${NX_SRC}/d3d_backend_gl.cpp"
    "${NX_SRC}/d3d_presenter_thread_nx.cpp" "${RUNTIME}/d3d_presenter_capture.cpp"
    "${RUNTIME}/d3d_vblank.cpp" "${RUNTIME}/xapi_clock.cpp" host_stubs.cpp)
target_include_directories(gl_backend_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/inc"
    "${egl_registry_SOURCE_DIR}/api" "${gl_registry_SOURCE_DIR}/api")
find_package(Threads REQUIRED)
target_link_libraries(gl_backend_test PRIVATE shader_path ${EGL_LIBRARY} ${GL_LIBRARY} Threads::Threads)

add_executable(capture_trust_test capture_trust_test.cpp "${RUNTIME}/d3d_presenter_capture.cpp")
target_include_directories(capture_trust_test PRIVATE "${RUNTIME}")
