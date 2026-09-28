cmake_minimum_required(VERSION 3.22.1)

# Enable C, C++, and Assembly (ASM is REQUIRED for libffi .S files!)
project("wayland_desktop" C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

add_definitions(-D_GNU_SOURCE)

# Find wayland-scanner on host (checks PATH and AOSP host out)
find_program(WAYLAND_SCANNER NAMES wayland-scanner
        PATHS
        /usr/bin
        /usr/local/bin
        $ENV{ANDROID_BUILD_TOP}/out/host/linux-x86/bin
)

if(NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner not found! Install it via 'sudo apt install libwayland-bin' or build it in AOSP ('m wayland_scanner').")
endif()

# ==============================================================================
# 1. COMPILE LIBFFI FROM SOURCE (Replicates libffi/Android.bp)
# ==============================================================================
set(LIBFFI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libffi)

# GENERATE ffi_gen.h using gen_ffi_header.sh!
execute_process(
        COMMAND /bin/sh ${LIBFFI_DIR}/gen_ffi_header.sh
        INPUT_FILE ${LIBFFI_DIR}/include/ffi.h.in
        OUTPUT_FILE ${LIBFFI_DIR}/include/ffi_gen.h
        WORKING_DIRECTORY ${LIBFFI_DIR}
        RESULT_VARIABLE FFI_GEN_RES
        ERROR_VARIABLE FFI_GEN_ERR
)

# Check if it succeeded
if(NOT FFI_GEN_RES EQUAL 0)
    message(FATAL_ERROR "ffi_gen.h generation failed! Exit code: ${FFI_GEN_RES}\nError: ${FFI_GEN_ERR}")
endif()

set(LIBFFI_SRCS
        ${LIBFFI_DIR}/src/closures.c
        ${LIBFFI_DIR}/src/debug.c
        ${LIBFFI_DIR}/src/java_raw_api.c
        ${LIBFFI_DIR}/src/prep_cif.c
        ${LIBFFI_DIR}/src/raw_api.c
        ${LIBFFI_DIR}/src/types.c
)

if(ANDROID_ABI STREQUAL "arm64-v8a")
    list(APPEND LIBFFI_SRCS
            ${LIBFFI_DIR}/src/aarch64/sysv.S
            ${LIBFFI_DIR}/src/aarch64/ffi.c
    )
    set(LIBFFI_ARCH_INC ${LIBFFI_DIR}/linux-arm64)
elseif(ANDROID_ABI STREQUAL "x86_64")
    list(APPEND LIBFFI_SRCS
            ${LIBFFI_DIR}/src/x86/unix64.S
            ${LIBFFI_DIR}/src/x86/win64.S
            ${LIBFFI_DIR}/src/x86/ffi64.c
            ${LIBFFI_DIR}/src/x86/ffiw64.c
    )
    set(LIBFFI_ARCH_INC ${LIBFFI_DIR}/linux-x86_64)
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    list(APPEND LIBFFI_SRCS
            ${LIBFFI_DIR}/src/arm/sysv.S
            ${LIBFFI_DIR}/src/arm/ffi.c
    )
    set(LIBFFI_ARCH_INC ${LIBFFI_DIR}/linux-arm)
elseif(ANDROID_ABI STREQUAL "x86")
    list(APPEND LIBFFI_SRCS
            ${LIBFFI_DIR}/src/x86/sysv.S
            ${LIBFFI_DIR}/src/x86/ffi.c
    )
    set(LIBFFI_ARCH_INC ${LIBFFI_DIR}/linux-x86)
endif()

add_library(ffi STATIC ${LIBFFI_SRCS})

target_include_directories(ffi PUBLIC
        ${LIBFFI_DIR}/include
        ${LIBFFI_ARCH_INC}
)

target_compile_options(ffi PRIVATE
        -Wno-deprecated-declarations
        -Wno-unused-parameter
)

# ==============================================================================
# 2. COMPILE LIBXKBCOMMON FROM SOURCE (Replicates libxkbcommon/Android.bp)
# ==============================================================================
set(XKB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libxkbcommon)

set(XKB_SRCS
        ${XKB_DIR}/config/libxkbcommon.so.0.0.0.p/parser.c
        ${XKB_DIR}/src/compose/parser.c
        ${XKB_DIR}/src/compose/paths.c
        ${XKB_DIR}/src/compose/state.c
        ${XKB_DIR}/src/compose/table.c
        ${XKB_DIR}/src/xkbcomp/action.c
        ${XKB_DIR}/src/xkbcomp/ast-build.c
        ${XKB_DIR}/src/xkbcomp/compat.c
        ${XKB_DIR}/src/xkbcomp/expr.c
        ${XKB_DIR}/src/xkbcomp/include.c
        ${XKB_DIR}/src/xkbcomp/keycodes.c
        ${XKB_DIR}/src/xkbcomp/keymap.c
        ${XKB_DIR}/src/xkbcomp/keymap-dump.c
        ${XKB_DIR}/src/xkbcomp/keywords.c
        ${XKB_DIR}/src/xkbcomp/rules.c
        ${XKB_DIR}/src/xkbcomp/scanner.c
        ${XKB_DIR}/src/xkbcomp/symbols.c
        ${XKB_DIR}/src/xkbcomp/types.c
        ${XKB_DIR}/src/xkbcomp/vmod.c
        ${XKB_DIR}/src/xkbcomp/xkbcomp.c
        ${XKB_DIR}/src/atom.c
        ${XKB_DIR}/src/context.c
        ${XKB_DIR}/src/context-priv.c
        ${XKB_DIR}/src/keymap.c
        ${XKB_DIR}/src/keymap-priv.c
        ${XKB_DIR}/src/keysym.c
        ${XKB_DIR}/src/keysym-utf.c
        ${XKB_DIR}/src/state.c
        ${XKB_DIR}/src/text.c
        ${XKB_DIR}/src/utf8.c
        ${XKB_DIR}/src/util-list.c
        ${XKB_DIR}/src/utils.c
)

add_library(xkbcommon STATIC ${XKB_SRCS})

target_include_directories(xkbcommon PUBLIC
        ${XKB_DIR}/include
        ${XKB_DIR}/src
        ${XKB_DIR}/config
        ${XKB_DIR}/config/libxkbcommon.so.0.0.0.p
)

target_compile_definitions(xkbcommon PRIVATE
        DFLT_XKB_CONFIG_ROOT="/usr/share/X11/xkb"
)

target_compile_options(xkbcommon PRIVATE
        -Wno-unused-parameter
        -Wno-missing-field-initializers
)

# ==============================================================================
# 3. COMPILE LIBWAYLAND FROM SOURCE (Replicates wayland/Android.bp)
# ==============================================================================
set(WAYLAND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wayland)
set(WAYLAND_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland_gen)
file(MAKE_DIRECTORY ${WAYLAND_GEN_DIR})

# Generate core wayland protocol headers & code using wayland-scanner!
execute_process(
        COMMAND ${WAYLAND_SCANNER} server-header ${WAYLAND_DIR}/protocol/wayland.xml ${WAYLAND_GEN_DIR}/wayland-server-protocol.h
)
execute_process(
        COMMAND ${WAYLAND_SCANNER} client-header ${WAYLAND_DIR}/protocol/wayland.xml ${WAYLAND_GEN_DIR}/wayland-client-protocol.h
)
execute_process(
        COMMAND ${WAYLAND_SCANNER} private-code ${WAYLAND_DIR}/protocol/wayland.xml ${WAYLAND_GEN_DIR}/wayland-protocol-private.c
)

# Wayland Server
add_library(wayland_server STATIC
        ${WAYLAND_DIR}/src/connection.c
        ${WAYLAND_DIR}/src/wayland-os.c
        ${WAYLAND_DIR}/src/wayland-util.c
        ${WAYLAND_DIR}/src/wayland-server.c
        ${WAYLAND_DIR}/src/wayland-shm.c
        ${WAYLAND_DIR}/src/event-loop.c
        ${WAYLAND_GEN_DIR}/wayland-protocol-private.c
)

target_include_directories(wayland_server PUBLIC
        ${WAYLAND_DIR}
        ${WAYLAND_DIR}/src
        ${WAYLAND_GEN_DIR}
)
target_link_libraries(wayland_server PUBLIC ffi)

# Wayland Client
add_library(wayland_client STATIC
        ${WAYLAND_DIR}/src/connection.c
        ${WAYLAND_DIR}/src/wayland-client.c
        ${WAYLAND_DIR}/src/wayland-os.c
        ${WAYLAND_DIR}/src/wayland-util.c
        ${WAYLAND_GEN_DIR}/wayland-protocol-private.c
)

target_include_directories(wayland_client PUBLIC
        ${WAYLAND_DIR}
        ${WAYLAND_DIR}/src
        ${WAYLAND_GEN_DIR}
)
target_link_libraries(wayland_client PUBLIC ffi)

# ==============================================================================
# 4. TARGET: WAYLAND BRIDGE JNI (libwayland_bridge.so)
# ==============================================================================
add_library(wayland_bridge SHARED
        bridge/wayland_bridge.cpp
        bridge/xdg-shell-protocol.c
)

target_include_directories(wayland_bridge PRIVATE
        bridge
        ${WAYLAND_DIR}/src
        ${WAYLAND_GEN_DIR}
)

target_link_libraries(wayland_bridge
        wayland_server
        android
        log
)

# ==============================================================================
# TARGET: TERMUX BOOTSTRAP (libtermux-bootstrap.so)
# ==============================================================================
add_library(termux-bootstrap SHARED
        termux-bootstrap-zip.S
        termux-bootstrap.c
)

# Ensures the output is named libtermux-bootstrap.so
set_target_properties(termux-bootstrap PROPERTIES
        OUTPUT_NAME "termux-bootstrap"
)

# Needed if termux-bootstrap-zip.S uses .incbin to locate the bootstrap zip
target_include_directories(termux-bootstrap PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(termux-bootstrap
        log
)
