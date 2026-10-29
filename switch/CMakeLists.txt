# SPDX-License-Identifier: GPL-3.0-or-later
# DOA2U NX: Nintendo Switch build of the doa2u-re runner (libnx .nro).
#
# Configure with devkitPro's toolchain file and the program the desktop
# build already generated and authenticated (build_game.sh):
#
#   cmake -S switch -B build/switch \
#     -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
#     -DRECOMP_PROGRAM_DIR=private/program/generated \
#     -DRECOMP_PROGRAM_MANIFEST_SHA256=<manifest> -DRECOMP_XBE_SHA256=<xbe>
#
# switch/build_switch.sh fills these in from private/program/receipt.json.
# The runtime sources are shared with recomp-runtime/; only the host layer
# (video, audio, input, entry point) lives in switch/src.
cmake_minimum_required(VERSION 3.20)

project(doa2u_nx C CXX ASM)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake")
endif()

set(DOA2U_NX_VERSION "0.2.0" CACHE STRING "Version shown in the homebrew menu")
set(DOA2U_NX_ICON "" CACHE FILEPATH "Optional 256x256 JPEG icon for the .nro")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# newlib hides some POSIX declarations under strict ISO modes.
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../recomp-runtime")
set(XBE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xbe")
set(NX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Same code generation rules as the desktop builds (see recomp-runtime):
# IEEE operation order, no strict aliasing, 32-bit wraparound.
add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>
    $<$<COMPILE_LANGUAGE:C,CXX>:-fno-strict-aliasing>
    $<$<COMPILE_LANGUAGE:C,CXX>:-fwrapv>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>)

set(RECOMP_PROGRAM_DIR "" CACHE PATH "Authenticated complete generated-source snapshot")
set(RECOMP_PROGRAM_MANIFEST_SHA256 "" CACHE STRING "SHA-256 of the generated-source manifest")
set(RECOMP_XBE_SHA256 "" CACHE STRING "SHA-256 of the executable used to generate this program")
if(NOT RECOMP_PROGRAM_DIR OR NOT RECOMP_PROGRAM_MANIFEST_SHA256)
    message(FATAL_ERROR "RECOMP_PROGRAM_DIR and RECOMP_PROGRAM_MANIFEST_SHA256 are required")
endif()
string(LENGTH "${RECOMP_XBE_SHA256}" _xbe_sha256_length)
if(NOT _xbe_sha256_length EQUAL 64 OR NOT RECOMP_XBE_SHA256 MATCHES "^[0-9A-Fa-f]+$")
    message(FATAL_ERROR "RECOMP_XBE_SHA256 must authenticate the generated program's executable")
endif()
string(TOLOWER "${RECOMP_XBE_SHA256}" _xbe_sha256)

# ---------------------------------------------------------------------------
# Generated program: authenticate and stub exactly as
# recomp-runtime/CMakeLists.txt does for the desktop runner.
# ---------------------------------------------------------------------------
get_filename_component(_recomp_program_dir "${RECOMP_PROGRAM_DIR}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
file(GLOB _recomp_program_names LIST_DIRECTORIES false
    RELATIVE "${_recomp_program_dir}"
    "${_recomp_program_dir}/recomp_[0-9][0-9][0-9][0-9].c")
list(SORT _recomp_program_names)
if(NOT _recomp_program_names)
    message(FATAL_ERROR "Authenticated program has no source chunks in ${_recomp_program_dir}")
endif()
set(_chunk_index 0)
foreach(_name IN LISTS _recomp_program_names)
    string(REGEX REPLACE "^recomp_0*([0-9]+)\\.c$" "\\1" _number "${_name}")
    if(NOT _number EQUAL _chunk_index)
        message(FATAL_ERROR "Authenticated program chunk sequence has a gap: ${_name}")
    endif()
    math(EXPR _chunk_index "${_chunk_index} + 1")
endforeach()
list(APPEND _recomp_program_names recomp_dispatch.c recomp_funcs.h)
set(_recomp_program_sources)
set(_recomp_program_manifest "")
foreach(_name IN LISTS _recomp_program_names)
    set(_path "${_recomp_program_dir}/${_name}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Authenticated program member is missing: ${_name}")
    endif()
    file(SIZE "${_path}" _size)
    file(SHA256 "${_path}" _sha256)
    string(APPEND _recomp_program_manifest "${_name}\t${_size}\t${_sha256}\n")
    # Compile copies beside the build, as upstream's runtime-sources folder
    # does: the program directory also holds the lifter's recomp_types.h,
    # which a quoted include would find before the runtime's.
    configure_file("${_path}" "${CMAKE_CURRENT_BINARY_DIR}/program/${_name}" COPYONLY)
    if(_name MATCHES "\\.c$")
        list(APPEND _recomp_program_sources "${CMAKE_CURRENT_BINARY_DIR}/program/${_name}")
    endif()
endforeach()
string(SHA256 _recomp_program_manifest_sha256 "${_recomp_program_manifest}")
string(TOLOWER "${RECOMP_PROGRAM_MANIFEST_SHA256}" _expected_program_manifest_sha256)
if(NOT _recomp_program_manifest_sha256 STREQUAL _expected_program_manifest_sha256)
    message(FATAL_ERROR "RECOMP_PROGRAM_DIR does not match its authenticated manifest")
endif()

set(_recomp_program_definitions)
set(_recomp_program_references)
foreach(_source IN LISTS _recomp_program_sources)
    file(READ "${_source}" _source_text)
    # Upstream refuses an uninitialized EBP prologue: the lifter fixes it.
    if(_source_text MATCHES "\n[ \t]+uint32_t ebp;[ \r]*\n")
        message(FATAL_ERROR "Authenticated program has an uninitialized EBP prologue")
    endif()
    string(REGEX MATCHALL
        "void[ \t\r\n]+sub_[0-9A-Fa-f]+[ \t\r\n]*\\([ \t\r\n]*void[ \t\r\n]*\\)[ \t\r\n]*\\{"
        _matches "${_source_text}")
    foreach(_match IN LISTS _matches)
        string(REGEX MATCH "sub_[0-9A-Fa-f]+" _name "${_match}")
        list(APPEND _recomp_program_definitions "${_name}")
    endforeach()
    string(REGEX MATCHALL "sub_[0-9A-Fa-f]+" _references "${_source_text}")
    list(APPEND _recomp_program_references ${_references})
endforeach()
list(REMOVE_DUPLICATES _recomp_program_definitions)
list(REMOVE_DUPLICATES _recomp_program_references)
set(_missing_source "${CMAKE_CURRENT_BINARY_DIR}/recomp_program_missing.c")
set(_missing_text "#include <stdint.h>\nvoid recomp_program_missing(uint32_t);\n")
set(_missing_count 0)
foreach(_name IN LISTS _recomp_program_references)
    list(FIND _recomp_program_definitions "${_name}" _defined_index)
    if(_defined_index EQUAL -1)
        string(SUBSTRING "${_name}" 4 -1 _address)
        string(APPEND _missing_text
            "void ${_name}(void) { recomp_program_missing(0x${_address}u); }\n")
        math(EXPR _missing_count "${_missing_count} + 1")
    endif()
endforeach()
# Rewrite only on change, so reconfiguring does not relink for nothing.
set(_old_missing "")
if(EXISTS "${_missing_source}")
    file(READ "${_missing_source}" _old_missing)
endif()
if(NOT _old_missing STREQUAL _missing_text)
    file(WRITE "${_missing_source}" "${_missing_text}")
endif()
message(STATUS "Generated ${_missing_count} fail-loud stubs for absent snapshot bodies")

# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------
set(_runtime_sources
    # recomp_program_runner (recomp-runtime/CMakeLists.txt), minus the
    # Windows/SDL input host, which input_host_switch.c replaces
    runner.cpp program_adapters.c program_manual.c
    d3d8_adapter.c d3d8_state_adapter.c d3d8_resource_adapter.c d3d8_draw_adapter.c
    d3d_device_model.c d3d_light_model.c d3d_render_state_model.c d3d_card_model.c
    d3d_frame_model.c d3d_tile_model.c d3d_vertex_shader_model.c
    dsound_api_adapter.c dsound_device_model.c dsound_buffer_adapter.c
    dsound_buffer_model.c dsound_effects_adapter.c dsound_effects_image.c dsound_mix_model.c
    xbox_adpcm.c
    input_model.c input_adapter.c input_pulse_source.c
    host_diagnostics.cpp
    # recomp_kernel
    runtime.c stop_report.c guest_heap.c xapi_clock.cpp native_fiber.c
    fiber_model.c fiber_adapter.c
    kernel_config.c kernel_crypto.c kernel_debug.c kernel_device.c
    kernel_dispatch.c kernel_file.c kernel_interrupt.c kernel_memory.c
    kernel_ordinals.c kernel_rtl.c kernel_startup.c kernel_thread.c kernel_video.c
    device_model.c apu_model.c ohci_model.c directory_model.c symbolic_link_model.c
    save_transaction.cpp win32_compat.c fiber_switch_arm64.S
    # recomp_xbe
    runner_load.cpp
    # recomp_audio (SDL backend minus SDL: audio_output_switch.cpp replaces it)
    audio_reverb.cpp dsound_hrtf.c
    # recomp_d3d_presenter's shared draw pieces; the presenter driver and
    # backend come with DOA2U_NX_PRESENTER below
    d3d_draw_model.c d3d_texture_model.c d3d_vblank.cpp)
list(TRANSFORM _runtime_sources PREPEND "${RUNTIME}/")

# gl: the OpenGL 4.3 renderer (stage 2) on its own thread on core 1, behind
# a portable copy of upstream's threaded presenter driver. probe: no
# rendering, the CPU-only measuring backend of stage 1, on the guest thread.
set(DOA2U_NX_PRESENTER "gl" CACHE STRING "Presenter backend: gl or probe")
if(DOA2U_NX_PRESENTER STREQUAL "probe")
    set(DOA2U_NX_PRESENTER_SOURCES
        "${NX_SRC}/d3d_backend_probe.cpp"
        "${RUNTIME}/d3d_presenter_inline.cpp")
    set(DOA2U_NX_PRESENTER_LIBS)
elseif(DOA2U_NX_PRESENTER STREQUAL "gl")
    # The draw shaders are HLSL generated at run time (d3d_draw_shader.cpp);
    # glslang and SPIRV-Cross, built here from pinned sources, turn them into
    # GLSL on the console (d3d_shader_compiler.cpp, RECOMP_SHADER_GLSLANG).
    # Their own build systems are not used: only their sources are fetched.
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(nx_glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG f0bd0257c308b9a26562c1a30c4748a0219cc951  # 16.2.0
        GIT_SHALLOW FALSE
        SOURCE_SUBDIR doa2u-nx-sources-only)
    FetchContent_Declare(nx_spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG a0fba56c34a6700f1724bf9b751da5b488a3775c  # vulkan-sdk-1.4.341.0
        GIT_SHALLOW FALSE
        SOURCE_SUBDIR doa2u-nx-sources-only)
    FetchContent_MakeAvailable(nx_glslang nx_spirv_cross)
    set(_glslang "${nx_glslang_SOURCE_DIR}")
    set(_spvc "${nx_spirv_cross_SOURCE_DIR}")
    # The installed include layout the compiler expects: glslang/SPIRV/*.h,
    # glslang/build_info.h and spirv_cross/*.hpp next to the source tree's glslang/.
    set(_shader_include "${CMAKE_CURRENT_BINARY_DIR}/shader-include")
    file(GLOB _glslang_spirv_headers "${_glslang}/SPIRV/*.h" "${_glslang}/SPIRV/*.hpp")
    file(COPY ${_glslang_spirv_headers} DESTINATION "${_shader_include}/glslang/SPIRV")
    file(GLOB _spvc_headers "${_spvc}/*.h" "${_spvc}/*.hpp")
    file(COPY ${_spvc_headers} DESTINATION "${_shader_include}/spirv_cross")
    file(WRITE "${_shader_include}/glslang/build_info.h.in"
        "#ifndef GLSLANG_BUILD_INFO\n#define GLSLANG_BUILD_INFO\n"
        "#define GLSLANG_VERSION_MAJOR 16\n#define GLSLANG_VERSION_MINOR 2\n"
        "#define GLSLANG_VERSION_PATCH 0\n#define GLSLANG_VERSION_FLAVOR \"\"\n"
        "#define GLSLANG_VERSION_GREATER_THAN(major, minor, patch) ((GLSLANG_VERSION_MAJOR) > (major) || ((major) == GLSLANG_VERSION_MAJOR && ((GLSLANG_VERSION_MINOR) > (minor) || ((minor) == GLSLANG_VERSION_MINOR && (GLSLANG_VERSION_PATCH) > (patch)))))\n"
        "#define GLSLANG_VERSION_GREATER_OR_EQUAL_TO(major, minor, patch) ((GLSLANG_VERSION_MAJOR) > (major) || ((major) == GLSLANG_VERSION_MAJOR && ((GLSLANG_VERSION_MINOR) > (minor) || ((minor) == GLSLANG_VERSION_MINOR && (GLSLANG_VERSION_PATCH >= (patch))))))\n"
        "#define GLSLANG_VERSION_LESS_THAN(major, minor, patch) ((GLSLANG_VERSION_MAJOR) < (major) || ((major) == GLSLANG_VERSION_MAJOR && ((GLSLANG_VERSION_MINOR) < (minor) || ((minor) == GLSLANG_VERSION_MINOR && (GLSLANG_VERSION_PATCH) < (patch)))))\n"
        "#define GLSLANG_VERSION_LESS_OR_EQUAL_TO(major, minor, patch) ((GLSLANG_VERSION_MAJOR) < (major) || ((major) == GLSLANG_VERSION_MAJOR && ((GLSLANG_VERSION_MINOR) < (minor) || ((minor) == GLSLANG_VERSION_MINOR && (GLSLANG_VERSION_PATCH <= (patch))))))\n"
        "#endif\n")
    configure_file("${_shader_include}/glslang/build_info.h.in" "${_shader_include}/glslang/build_info.h" COPYONLY)

    set(_glslang_sources
        SPIRV/GlslangToSpv.cpp SPIRV/InReadableOrder.cpp SPIRV/Logger.cpp SPIRV/SpvBuilder.cpp
        SPIRV/SpvPostProcess.cpp SPIRV/SpvTools.cpp SPIRV/disassemble.cpp SPIRV/doc.cpp
        glslang/GenericCodeGen/CodeGen.cpp glslang/GenericCodeGen/Link.cpp
        glslang/HLSL/hlslAttributes.cpp glslang/HLSL/hlslGrammar.cpp glslang/HLSL/hlslOpMap.cpp
        glslang/HLSL/hlslParseHelper.cpp glslang/HLSL/hlslParseables.cpp glslang/HLSL/hlslScanContext.cpp
        glslang/HLSL/hlslTokenStream.cpp
        glslang/MachineIndependent/Constant.cpp glslang/MachineIndependent/InfoSink.cpp
        glslang/MachineIndependent/Initialize.cpp glslang/MachineIndependent/IntermTraverse.cpp
        glslang/MachineIndependent/Intermediate.cpp glslang/MachineIndependent/ParseContextBase.cpp
        glslang/MachineIndependent/ParseHelper.cpp glslang/MachineIndependent/PoolAlloc.cpp
        glslang/MachineIndependent/RemoveTree.cpp glslang/MachineIndependent/Scan.cpp
        glslang/MachineIndependent/ShaderLang.cpp glslang/MachineIndependent/SpirvIntrinsics.cpp
        glslang/MachineIndependent/SymbolTable.cpp glslang/MachineIndependent/Versions.cpp
        glslang/MachineIndependent/attribute.cpp glslang/MachineIndependent/glslang_tab.cpp
        glslang/MachineIndependent/intermOut.cpp glslang/MachineIndependent/iomapper.cpp
        glslang/MachineIndependent/limits.cpp glslang/MachineIndependent/linkValidate.cpp
        glslang/MachineIndependent/parseConst.cpp glslang/MachineIndependent/preprocessor/Pp.cpp
        glslang/MachineIndependent/preprocessor/PpAtom.cpp glslang/MachineIndependent/preprocessor/PpContext.cpp
        glslang/MachineIndependent/preprocessor/PpScanner.cpp glslang/MachineIndependent/preprocessor/PpTokens.cpp
        glslang/MachineIndependent/propagateNoContraction.cpp glslang/MachineIndependent/reflection.cpp
        glslang/OSDependent/Unix/ossource.cpp glslang/ResourceLimits/ResourceLimits.cpp)
    list(TRANSFORM _glslang_sources PREPEND "${_glslang}/")
    add_library(nx_glslang STATIC ${_glslang_sources})
    # PRIVATE throughout: anything these libraries exported to doa2u-nx would
    # change every generated chunk's command line and recompile them all.
    target_compile_definitions(nx_glslang PRIVATE ENABLE_HLSL ENABLE_OPT=0 ENABLE_SPIRV GLSLANG_OSINCLUDE_UNIX)
    target_include_directories(nx_glslang PRIVATE "${_shader_include}" "${_glslang}")
    target_compile_options(nx_glslang PRIVATE -O2 -w)
    add_library(nx_spirv_cross STATIC
        "${_spvc}/spirv_cfg.cpp" "${_spvc}/spirv_cross.cpp" "${_spvc}/spirv_cross_parsed_ir.cpp"
        "${_spvc}/spirv_glsl.cpp" "${_spvc}/spirv_parser.cpp")
    target_include_directories(nx_spirv_cross PRIVATE "${_shader_include}")
    target_compile_options(nx_spirv_cross PRIVATE -O2 -w)

    set(DOA2U_NX_PRESENTER_SOURCES
        "${NX_SRC}/d3d_backend_gl.cpp"
        "${NX_SRC}/d3d_presenter_thread_nx.cpp"
        "${RUNTIME}/d3d_presenter_capture.cpp"
        "${RUNTIME}/d3d_shader_compiler.cpp"
        "${RUNTIME}/d3d_draw_shader.cpp"
        "${RUNTIME}/d3d_vertex_program.cpp")
    set_source_files_properties("${RUNTIME}/d3d_shader_compiler.cpp" PROPERTIES
        COMPILE_DEFINITIONS RECOMP_SHADER_GLSLANG
        INCLUDE_DIRECTORIES "${_shader_include};${_glslang}")
    # devkitPro switch-mesa and switch-glad (switch-portlibs).
    set(DOA2U_NX_PRESENTER_LIBS nx_spirv_cross nx_glslang glad EGL glapi drm_nouveau)
else()
    message(FATAL_ERROR "Unknown DOA2U_NX_PRESENTER '${DOA2U_NX_PRESENTER}'")
endif()

add_executable(doa2u-nx
    ${_runtime_sources}
    "${NX_SRC}/nx_main.cpp"
    "${NX_SRC}/switch_shims.c"
    "${NX_SRC}/boot_trace.cpp"
    "${NX_SRC}/crash_report.cpp"
    "${NX_SRC}/profile.cpp"
    "${NX_SRC}/input_host_switch.c"
    "${NX_SRC}/audio_output_switch.cpp"
    ${DOA2U_NX_PRESENTER_SOURCES}
    "${_missing_source}"
    ${_recomp_program_sources}
    "${XBE_DIR}/sha.cpp"
    "${XBE_DIR}/xbe_parser.cpp")

target_compile_definitions(doa2u-nx PRIVATE
    RECOMP_XBE_SHA256="${_xbe_sha256}"
    DOA2U_NX_VERSION="${DOA2U_NX_VERSION}")
# DirectSound drops audio spans after a longer gap between the game's pumps.
# Bring-up frames still take 100-150 ms, so keep their audio instead of
# dropping nearly all of it. Set on the one file that reads it, so changing it
# never recompiles the generated program.
set_source_files_properties("${RUNTIME}/dsound_buffer_model.c" PROPERTIES
    COMPILE_DEFINITIONS "RECOMP_DSOUND_STALL_MS=250u")
# switch/include comes first: it supplies the POSIX headers newlib lacks.
target_include_directories(doa2u-nx BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${RUNTIME}"
    "${XBE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/program")
set_source_files_properties("${RUNTIME}/runner.cpp" PROPERTIES
    COMPILE_DEFINITIONS "main=recomp_runner_main")
# The generated chunks are ~140k lines each; -O2 keeps the build time sane
# while still giving the register allocation the lifted code needs.
set_source_files_properties(${_recomp_program_sources} PROPERTIES
    COMPILE_OPTIONS "-O2;-w")
target_compile_options(doa2u-nx PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
target_link_options(doa2u-nx PRIVATE "-Wl,--wrap=exit" "-Wl,--wrap=abort" "-Wl,--wrap=setvbuf" "-Wl,--wrap=pthread_create"
    "-Wl,--wrap=recomp_install_host_diagnostics" "-Wl,--wrap=recomp_save_initialize"
    "-Wl,--wrap=recomp_runtime_init"
    "-Wl,--wrap=recomp_dispatch"
    "-Wl,--wrap=recomp_lookup_manual" "-Wl,--wrap=recomp_lookup_kernel"
    "-Wl,--wrap=recomp_program_lookup" "-Wl,--wrap=recomp_native_fiber_switch"
    "-Wl,--wrap=nanosleep" "-Wl,--wrap=recomp_stop_note_kernel_call" "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/doa2u-nx.map")
target_include_directories(doa2u-nx SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include")
target_link_libraries(doa2u-nx PRIVATE ${DOA2U_NX_PRESENTER_LIBS} nx m)

nx_generate_nacp(OUTPUT doa2u-nx.nacp
    NAME "DOA2U NX"
    AUTHOR "Toniisound"
    VERSION "${DOA2U_NX_VERSION}")
if(DOA2U_NX_ICON)
    nx_create_nro(doa2u-nx NACP doa2u-nx.nacp ICON "${DOA2U_NX_ICON}")
else()
    nx_create_nro(doa2u-nx NACP doa2u-nx.nacp)
endif()
