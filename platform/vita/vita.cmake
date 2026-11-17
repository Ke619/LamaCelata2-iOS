# The PS Vita build (included by the top CMakeLists when the VitaSDK toolchain
# is used): the runtime, and for now the bench (tests/bench), as a homebrew
# app. The translator runs on the Mac, so the translated sources come from a
# Mac build (SCIMITAR_HOST_BUILD, its `bench_sources` target). See
# platform/vita/README.md.
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(SCIMITAR_HOST_BUILD "${CMAKE_SOURCE_DIR}/build"
    CACHE PATH "A Mac build directory that has built the bench_sources target")
set(BENCH_GEN ${SCIMITAR_HOST_BUILD}/gen/bench_kernels.cpp)
set(LZO_GEN ${SCIMITAR_HOST_BUILD}/gen/lzo1x_decompress_safe.cpp)
set(BENCH_EXE ${SCIMITAR_HOST_BUILD}/bench/kernels.exe)
foreach(f ${BENCH_GEN} ${LZO_GEN} ${BENCH_EXE})
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "${f} is missing: build the bench_sources target of the Mac build "
                            "(${SCIMITAR_HOST_BUILD}) first, or set SCIMITAR_HOST_BUILD")
    endif()
endforeach()

# Cortex-A9: Thumb-2, NEON (unused by the runtime: ARMv7's NEON flushes
# denormals, so SSE lanes are computed on VFP, which is IEEE like x86).
add_compile_options(-mcpu=cortex-a9 -mthumb -mfpu=neon -mfloat-abi=hard)

add_library(scimitar_macho STATIC recomp/src/macho.cpp recomp/src/pe.cpp)
target_include_directories(scimitar_macho PUBLIC recomp/src)

add_library(scimitar_runtime STATIC ${SCIMITAR_RUNTIME_SOURCES})
target_include_directories(scimitar_runtime PUBLIC runtime/include)
target_link_libraries(scimitar_runtime PUBLIC scimitar_macho)

set(VITA_BENCH_TITLEID "SCIM00001" CACHE STRING "The bench app's title ID")
add_executable(scimitar-bench tests/bench/bench.cpp tests/bench/main_vita.cpp tests/bench/kernels.c
               ${BENCH_GEN} ${LZO_GEN})
set_source_files_properties(tests/bench/kernels.c PROPERTIES COMPILE_OPTIONS
                            "-O2;-fno-math-errno;-fno-tree-vectorize")
set_source_files_properties(${BENCH_GEN} ${LZO_GEN} PROPERTIES COMPILE_OPTIONS "-O2;-fwrapv-pointer")
# The whole of pthread: libstdc++ finds out whether threads exist from weak
# references to pthread functions, and with only the referenced objects
# linked it decides they don't (std::call_once then throws, std::mutex
# doesn't lock).
set(VITA_PTHREAD -Wl,--whole-archive pthread -Wl,--no-whole-archive)
target_link_libraries(scimitar-bench PRIVATE scimitar_runtime ${VITA_PTHREAD} SceKernelThreadMgr_stub
                      ScePower_stub SceIofilemgr_stub SceLibKernel_stub SceSysmem_stub)
# Progress on the Vita's screen: the debug console from VitaSDK's samples,
# compiled from the SDK in place.
set(VITA_DEBUG_SCREEN "$ENV{VITASDK}/share/gcc-arm-vita-eabi/samples/common")
if(EXISTS "${VITA_DEBUG_SCREEN}/debugScreen.c")
    target_sources(scimitar-bench PRIVATE "${VITA_DEBUG_SCREEN}/debugScreen.c")
    set_source_files_properties("${VITA_DEBUG_SCREEN}/debugScreen.c" PROPERTIES COMPILE_OPTIONS "-std=gnu11;-w")
    target_include_directories(scimitar-bench PRIVATE "${VITA_DEBUG_SCREEN}")
    target_compile_definitions(scimitar-bench PRIVATE BENCH_SCREEN)
    target_link_libraries(scimitar-bench PRIVATE SceDisplay_stub)
endif()
vita_create_self(eboot.bin scimitar-bench)
vita_create_vpk(scimitar-bench.vpk ${VITA_BENCH_TITLEID} eboot.bin
    VERSION 01.00
    NAME "scimitar bench"
    FILE ${BENCH_EXE} kernels.exe
    FILE ${CMAKE_SOURCE_DIR}/tests/lzo/vectors/140.lzo 140.lzo)

# The translated game (SCIMITAR_VITA_GAME=ON): the gen/full sources of a Mac
# build with SCIMITAR_FULL_IMAGE=ON, compiled for the Vita. For now this
# measures their size and whether a module that large loads (ac2-loadtest).
option(SCIMITAR_VITA_GAME "Compile the translated AC2 for the Vita" OFF)
if(SCIMITAR_VITA_GAME)
    set(SCIMITAR_GEN_DIR "${SCIMITAR_HOST_BUILD}/gen/full"
        CACHE PATH "The translated game (gen/full of a Mac build with SCIMITAR_FULL_IMAGE=ON)")
    file(GLOB AC2_GEN_SOURCES "${SCIMITAR_GEN_DIR}/ac2_*.cpp")
    if(NOT AC2_GEN_SOURCES)
        message(FATAL_ERROR "No translated game in ${SCIMITAR_GEN_DIR}")
    endif()
    add_library(ac2_full STATIC ${AC2_GEN_SOURCES})
    target_link_libraries(ac2_full PUBLIC scimitar_runtime)
    # Guest addresses are added to the window's base in 32-bit pointers: let
    # that wrap rather than be undefined (GCC otherwise bounds loops by it).
    set_source_files_properties(${AC2_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-fwrapv-pointer")

    # ac2-loadtest: the translated game in an eboot that only starts (title
    # SCIM00002), with stubs for the overrides the shell would implement.
    set(LOADTEST_STUBS ${CMAKE_BINARY_DIR}/loadtest_stubs.cpp)
    add_custom_command(
        OUTPUT ${LOADTEST_STUBS}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/platform/vita/stub_undefined.py ${CMAKE_NM} ${LOADTEST_STUBS}
                $<TARGET_FILE:ac2_full> $<TARGET_FILE:scimitar_runtime>
        DEPENDS ac2_full scimitar_runtime platform/vita/stub_undefined.py
        COMMENT "Stubbing the overrides for ac2-loadtest")
    add_executable(ac2-loadtest platform/vita/loadtest.cpp ${LOADTEST_STUBS})
    target_link_libraries(ac2-loadtest PRIVATE -Wl,--whole-archive ac2_full -Wl,--no-whole-archive
                          scimitar_runtime ${VITA_PTHREAD} SceLibKernel_stub SceKernelThreadMgr_stub
                          SceIofilemgr_stub SceSysmem_stub)
    vita_create_self(loadtest.bin ac2-loadtest)
    vita_create_vpk(ac2-loadtest.vpk SCIM00002 loadtest.bin VERSION 01.00 NAME "scimitar loadtest")
endif()
