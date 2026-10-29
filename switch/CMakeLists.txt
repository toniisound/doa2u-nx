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

set(DOA2U_NX_VERSION "0.1.0" CACHE STRING "Version shown in the homebrew menu")
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
    # recomp_d3d_presenter: the presenter API driver and shared draw pieces;
    # the backend comes from switch/src
    d3d_presenter_inline.cpp d3d_draw_model.c d3d_texture_model.c d3d_vblank.cpp)
list(TRANSFORM _runtime_sources PREPEND "${RUNTIME}/")

# probe: no rendering, the CPU-only measuring backend of stage 1. The
# OpenGL backend (stage 2) is added here as "gl".
set(DOA2U_NX_PRESENTER "probe" CACHE STRING "Presenter backend: probe")
if(DOA2U_NX_PRESENTER STREQUAL "probe")
    set(DOA2U_NX_PRESENTER_SOURCES "${NX_SRC}/d3d_backend_probe.cpp")
    set(DOA2U_NX_PRESENTER_LIBS)
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
