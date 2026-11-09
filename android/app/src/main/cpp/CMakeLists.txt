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
