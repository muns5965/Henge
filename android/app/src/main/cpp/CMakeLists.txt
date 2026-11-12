cmake_minimum_required(VERSION 3.22)
project(henge_probe C)

# The JIT mapping under test is the repository's own host/jit_region.c,
# compiled unchanged.
set(HENGE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

add_library(henge_probe SHARED
    probe_jni.c
    mt_jni.c
    mem_jni.c
    ${HENGE_ROOT}/host/jit_region.c)

target_include_directories(henge_probe PRIVATE ${HENGE_ROOT}/host)
target_compile_options(henge_probe PRIVATE -O1 -Wall -Wextra)
target_link_libraries(henge_probe log)

# Devices with 16 KB pages need 16 KB-aligned segments in the library.
target_link_options(henge_probe PRIVATE -Wl,-z,max-page-size=16384)

# C0 + B0 screen and input test (no virtual machine): the repository's host/display_buf.c, host/key_map.c, host/touch_input.c and host/render_gate.c, compiled unchanged. A library of its own,
# so the probe library above and the vmrun programs are not touched. keycheck.c checks the key table against the NDK's key codes at compile time.
add_library(henge_ui SHARED
    ui_jni.c
    keycheck.c
    ${HENGE_ROOT}/host/display_buf.c
    ${HENGE_ROOT}/host/key_map.c
    ${HENGE_ROOT}/host/touch_input.c
    ${HENGE_ROOT}/host/render_gate.c)

target_include_directories(henge_ui PRIVATE ${HENGE_ROOT}/host)
target_compile_options(henge_ui PRIVATE -O1 -Wall -Wextra -Werror)
target_link_libraries(henge_ui android log m)
target_link_options(henge_ui PRIVATE -Wl,-z,max-page-size=16384)
