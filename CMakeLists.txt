cmake_minimum_required(VERSION 3.20)
project(scimitar_recomp LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Floating-point results must match the original x86 code bit for bit.
add_compile_options(-ffp-contract=off)

enable_testing()

# The platform layer's sources; opengl.cpp (desktop GL, with the Metal backend
# as an option) is added per platform.
set(SCIMITAR_PLATFORM_SOURCES platform/src/posix_io.cpp platform/src/libc_mem.cpp
    platform/src/guest_heap.cpp platform/src/libc_string.cpp platform/src/libc_printf.cpp
    platform/src/pthread.cpp platform/src/mach_sync.cpp platform/src/libc_misc.cpp
    platform/src/objc.cpp platform/src/orbit.cpp platform/src/libc_math.cpp
    platform/src/libc_conv.cpp platform/src/libc_process.cpp platform/src/stdio.cpp
    platform/src/posix_fs.cpp platform/src/data_imports.cpp platform/src/bridge.cpp
    platform/src/cf.cpp platform/src/audio.cpp platform/src/iokit.cpp platform/src/carbon_fork.cpp
    platform/src/coregraphics.cpp platform/src/bink.cpp platform/src/d3d9_arb.cpp
    platform/src/arb_store.cpp)
set(SCIMITAR_METAL_SOURCES platform/src/metal/backend.mm platform/src/metal/shims.mm platform/src/metal/dump.mm platform/src/metal/shader_cache.mm)
set(SCIMITAR_RUNTIME_SOURCES runtime/src/guest_memory.cpp runtime/src/runtime.cpp
    runtime/src/image_loader.cpp runtime/src/x87.cpp runtime/src/sse.cpp)

# iOS (cmake -DCMAKE_SYSTEM_NAME=iOS ...): the port only, from a Mac build's
# translated game; see games/ac2/ios/README.md.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    include(games/ac2/ios/ios.cmake)
    return()
endif()

# PS Vita (cmake -DCMAKE_TOOLCHAIN_FILE=/opt/vitasdk/share/vita.toolchain.cmake ...):
# the bench, from a Mac build's translated kernels; see platform/vita/README.md.
if(VITA)
    include(platform/vita/vita.cmake)
    return()
endif()

add_library(scimitar_macho STATIC recomp/src/macho.cpp recomp/src/pe.cpp)
target_include_directories(scimitar_macho PUBLIC recomp/src)

add_library(scimitar_runtime STATIC ${SCIMITAR_RUNTIME_SOURCES})
target_include_directories(scimitar_runtime PUBLIC runtime/include)
target_link_libraries(scimitar_runtime PUBLIC scimitar_macho)

add_executable(guest_memory_test tests/runtime/guest_memory_test.cpp)
target_link_libraries(guest_memory_test PRIVATE scimitar_runtime)
add_test(NAME guest_memory COMMAND guest_memory_test)
add_test(NAME guest_memory_window COMMAND guest_memory_test)
set_tests_properties(guest_memory_window PROPERTIES ENVIRONMENT SCIMITAR_GUEST_WINDOW_MB=62)

# AC2's color filter patch (games/ac2/color_filter_patch.h) on look-alike records.
add_executable(color_filter_test tests/ac2/color_filter_test.cpp)
target_include_directories(color_filter_test PRIVATE games/ac2)
add_test(NAME color_filter COMMAND color_filter_test)

set(LZO_FIXTURE ${CMAKE_SOURCE_DIR}/fixtures/ac2-1.0.1/lzo1x_decompress_safe.bin)
set(LZO_FIXTURE_ADDR 0x0015c1c0)
set(LZO_VECTORS ${CMAKE_SOURCE_DIR}/tests/lzo/vectors)

# The translator.
find_package(zydis REQUIRED)
# Homebrew's Zydis config links "Zycore" by bare name, so locate it explicitly.
find_library(ZYCORE_LIBRARY Zycore REQUIRED)
get_filename_component(ZYCORE_LIBDIR ${ZYCORE_LIBRARY} DIRECTORY)
add_library(recomp_core STATIC recomp/src/flow.cpp recomp/src/emit.cpp)
target_link_libraries(recomp_core PUBLIC scimitar_macho)
target_link_directories(recomp_core PUBLIC ${ZYCORE_LIBDIR})
target_link_libraries(recomp_core PUBLIC Zydis::Zydis)

add_executable(scimitar-recomp recomp/src/main.cpp)
target_link_libraries(scimitar-recomp PRIVATE recomp_core)

add_executable(scimitar-analyze recomp/src/analyze_main.cpp)
target_link_libraries(scimitar-analyze PRIVATE recomp_core)

# Recompile the LZO fixture and test it against the original code's results.
set(LZO_GEN ${CMAKE_BINARY_DIR}/gen/lzo1x_decompress_safe.cpp)
add_custom_command(
    OUTPUT ${LZO_GEN}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
    COMMAND scimitar-recomp --bin ${LZO_FIXTURE} --addr ${LZO_FIXTURE_ADDR}
            --name lzo1x_decompress_safe --out ${LZO_GEN}
    DEPENDS scimitar-recomp ${LZO_FIXTURE}
    COMMENT "Recompiling lzo1x_decompress_safe")
add_executable(lzo_recomp_test tests/lzo/recomp_test.cpp ${LZO_GEN})
target_link_libraries(lzo_recomp_test PRIVATE scimitar_runtime)
add_test(NAME lzo_recomp COMMAND lzo_recomp_test ${LZO_VECTORS}
         ${LZO_VECTORS}/oracle-lzo2.01.tsv)

# scimitar-bench (tests/bench/README.md): synthetic kernels built as an i386
# Windows executable (needs MinGW's i686 GCC), translated, and run against the
# same kernels compiled natively; the PS Vita build runs the same files.
find_program(MINGW32_GCC i686-w64-mingw32-gcc)
if(MINGW32_GCC)
    set(BENCH_EXE ${CMAKE_BINARY_DIR}/bench/kernels.exe)
    set(BENCH_GEN ${CMAKE_BINARY_DIR}/gen/bench_kernels.cpp)
    set(BENCH_ROOTS bench_vecmath bench_particles bench_hash bench_sort bench_entities bench_tree
        bench_text bench_double walker_update guard_update prop_update walker_priority
        guard_priority prop_priority)
    set(bench_root_args)
    foreach(r ${BENCH_ROOTS})
        list(APPEND bench_root_args --root ${r})
    endforeach()
    # Like the game's compiler: SSE floating point, frame pointers, no vectorizing.
    add_custom_command(
        OUTPUT ${BENCH_EXE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bench
        COMMAND ${MINGW32_GCC} -DBENCH_GUEST -O2 -march=prescott -mfpmath=sse -fno-tree-vectorize
                -fno-omit-frame-pointer -fno-math-errno -fno-tree-loop-distribute-patterns
                -fno-asynchronous-unwind-tables -fno-stack-protector -mno-stack-arg-probe
                -ffreestanding -nostdlib -Wl,-e,_bench_entry -Wl,--image-base=0x00400000
                -Wl,--disable-dynamicbase -o ${BENCH_EXE} ${CMAKE_SOURCE_DIR}/tests/bench/kernels.c
        DEPENDS tests/bench/kernels.c
        COMMENT "Building the bench kernels for i386 Windows")
    add_custom_command(
        OUTPUT ${BENCH_GEN}
        COMMAND scimitar-recomp --image ${BENCH_EXE} ${bench_root_args} --out ${BENCH_GEN}
        DEPENDS scimitar-recomp ${BENCH_EXE}
        COMMENT "Translating the bench kernels")
    add_custom_target(bench_sources DEPENDS ${BENCH_EXE} ${BENCH_GEN} ${LZO_GEN})
    add_executable(scimitar-bench tests/bench/bench.cpp tests/bench/main_host.cpp
                   tests/bench/kernels.c ${BENCH_GEN} ${LZO_GEN})
    set_source_files_properties(tests/bench/kernels.c PROPERTIES COMPILE_OPTIONS
                                "-O2;-fno-math-errno;-fno-vectorize;-fno-slp-vectorize")
    set_source_files_properties(${BENCH_GEN} PROPERTIES COMPILE_OPTIONS "-O2")  # as the game's
    target_link_libraries(scimitar-bench PRIVATE scimitar_runtime)
    add_test(NAME bench_checksums COMMAND scimitar-bench ${BENCH_EXE} ${LZO_VECTORS}/140.lzo)
endif()

# Oracle: runs the original x86 fixture in Unicorn (optional dependency).
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(UNICORN IMPORTED_TARGET unicorn)
endif()
if(UNICORN_FOUND)
    add_executable(lzo_oracle tests/lzo/oracle_unicorn.cpp)
    target_link_libraries(lzo_oracle PRIVATE PkgConfig::UNICORN)
    add_test(NAME lzo_oracle COMMAND lzo_oracle ${LZO_FIXTURE} ${LZO_FIXTURE_ADDR}
             ${LZO_VECTORS} ${CMAKE_BINARY_DIR}/lzo_oracle_results.tsv)

    # The runtime's x87 arithmetic against Unicorn running real x87 instructions.
    add_executable(x87_test tests/x87/x87_test.cpp)
    target_link_libraries(x87_test PRIVATE scimitar_runtime PkgConfig::UNICORN)
    add_test(NAME x87 COMMAND x87_test)
endif()

# The Metal backend's ARB -> MSL translator (plain C++, see docs/METAL.md).
add_library(scimitar_arb_msl STATIC platform/src/arb_msl.cpp)
target_include_directories(scimitar_arb_msl PUBLIC platform/include)

add_library(scimitar_platform STATIC ${SCIMITAR_PLATFORM_SOURCES} platform/src/opengl.cpp)
target_include_directories(scimitar_platform PUBLIC platform/include)
if(APPLE)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(OPENGL_FW OpenGL REQUIRED)
    find_library(AUDIOTOOLBOX_FW AudioToolbox REQUIRED)
    find_library(AUDIOUNIT_FW AudioUnit REQUIRED)
    find_library(COREAUDIO_FW CoreAudio REQUIRED)
    find_library(APPSERVICES_FW ApplicationServices REQUIRED)
    target_link_libraries(scimitar_platform PUBLIC ${COREFOUNDATION_FRAMEWORK} ${OPENGL_FW}
                          ${AUDIOTOOLBOX_FW} ${AUDIOUNIT_FW} ${COREAUDIO_FW} ${APPSERVICES_FW})
    # The Metal rendering backend (SCIMITAR_RENDERER=metal).
    find_library(METAL_FW Metal REQUIRED)
    find_library(QUARTZCORE_FW QuartzCore REQUIRED)
    find_library(FOUNDATION_FW Foundation REQUIRED)
    target_sources(scimitar_platform PRIVATE ${SCIMITAR_METAL_SOURCES})
    set_source_files_properties(${SCIMITAR_METAL_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(scimitar_platform PUBLIC scimitar_arb_msl ${METAL_FW} ${QUARTZCORE_FW} ${FOUNDATION_FW})
endif()
target_link_libraries(scimitar_platform PUBLIC scimitar_runtime)
# Bink videos through FFmpeg (optional: without it the game skips its videos).
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
endif()
if(FFMPEG_FOUND)
    target_link_libraries(scimitar_platform PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(scimitar_platform PRIVATE SCIMITAR_HAVE_FFMPEG)
endif()

# The local replacement for the Orbit launcher library (saves), as the game drives it.
add_executable(orbit_test tests/orbit/orbit_test.cpp)
target_link_libraries(orbit_test PRIVATE scimitar_platform)
add_test(NAME orbit COMMAND orbit_test)

# Risk check: legacy OpenGL on this Mac must accept the game's ARB assembly programs.
if(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    add_executable(gl_arb_probe tests/gl/arb_probe.cpp)
    target_link_libraries(gl_arb_probe PRIVATE ${OPENGL_FRAMEWORK})
    add_test(NAME gl_arb COMMAND gl_arb_probe)

    # The Metal backend's ARB -> MSL translator, run on the GPU with synthetic programs.
    add_executable(arb_msl_test tests/metal/arb_msl_test.mm)
    set_source_files_properties(tests/metal/arb_msl_test.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(arb_msl_test PRIVATE scimitar_arb_msl ${METAL_FW} ${FOUNDATION_FW})
    add_test(NAME arb_msl COMMAND arb_msl_test)

    # D3D9 bytecode -> ARB (the Windows release's shaders, docs/PC_DATA.md):
    # compiles translated programs with OpenGL and the ARB -> MSL translator.
    add_library(scimitar_d3d9_arb STATIC platform/src/d3d9_arb.cpp)
    target_include_directories(scimitar_d3d9_arb PUBLIC platform/include)
    add_executable(d3d9_arb_check tools/d3d9_arb_check.mm)
    set_source_files_properties(tools/d3d9_arb_check.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(d3d9_arb_check PRIVATE scimitar_d3d9_arb scimitar_arb_msl ${OPENGL_FRAMEWORK} ${METAL_FW} ${FOUNDATION_FW})
    add_executable(d3d9_arb_test tests/d3d9/d3d9_arb_test.cpp)
    target_link_libraries(d3d9_arb_test PRIVATE scimitar_d3d9_arb scimitar_arb_msl ${OPENGL_FRAMEWORK})
    add_test(NAME d3d9_arb COMMAND d3d9_arb_test)
endif()

# liblzo2 (optional): regenerates LZO vectors and independently checks forge data.
find_library(LZO2_LIBRARY lzo2)
find_path(LZO2_INCLUDE_DIR lzo/lzo1x.h)

# Tests that need the game: translated from the local copy at AC2_APP, never
# committed. Skipped when the game is not there.
set(AC2_APP "${CMAKE_SOURCE_DIR}/../steam_depot/33362_5/Assassin's Creed 2.app"
    CACHE PATH "Local Assassin's Creed 2.app (Mac)")
set(AC2_BINARY "${AC2_APP}/Contents/MacOS/scimitar_sf")
if(EXISTS "${AC2_BINARY}" AND UNICORN_FOUND)
    set(FORGE_GEN ${CMAKE_BINARY_DIR}/gen/forge_readers.cpp)
    add_custom_command(
        OUTPUT ${FORGE_GEN}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
        COMMAND scimitar-recomp --image "${AC2_BINARY}"
                --root __ZN8scimitar7BigFile10HeaderFile4ReadEPS0_RNS_4FileE
                --root __ZN8scimitar10BigFileFat10ReadHeaderEPNS_7BigFileEm
                --root __ZN8scimitar4File4SeekExNS0_8SeekTypeE
                --root __ZN8scimitar21BigFileFatDescription4ReadEPNS_7BigFileERNS_4FileEmm
                --root __ZN8scimitar7FatFile4ReadEPNS_7BigFileERNS_4FileEm
                --root __ZN8scimitar7BigFile8ReadFileENS_12BigFileIndexERm
                --root __ZN8scimitar17CompressBinStream16DecompressBufferEPhmRS1_RmRN4Gear12MemInterfaceE
                --root __GLOBAL__I__ZN8scimitar27CreateXDecompressionContextEv
                --root _lzo1x_decompress
                --out ${FORGE_GEN}
        DEPENDS scimitar-recomp "${AC2_BINARY}"
        COMMENT "Recompiling the forge readers from the local AC2 binary"
        VERBATIM)
    add_executable(forge_test tests/forge/forge_test.cpp ${FORGE_GEN})
    target_link_libraries(forge_test PRIVATE scimitar_platform PkgConfig::UNICORN)
    if(LZO2_LIBRARY AND LZO2_INCLUDE_DIR)  # independent decode of the archive
        target_include_directories(forge_test PRIVATE ${LZO2_INCLUDE_DIR})
        target_link_libraries(forge_test PRIVATE ${LZO2_LIBRARY})
    endif()
    add_test(NAME forge COMMAND forge_test "${AC2_APP}")

    # Saves through the game's own OrbitClient code with the local Orbit
    # replacement (games/common/overrides.cpp replaces the launcher library loader).
    set(SAVES_GEN ${CMAKE_BINARY_DIR}/gen/saves.cpp)
    add_custom_command(
        OUTPUT ${SAVES_GEN}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
        COMMAND scimitar-recomp --image "${AC2_BINARY}" --weak-imports
                --root __GLOBAL__I__ZN8scimitar11OrbitClient11ms_InstanceE
                --root __GLOBAL__I__ZN2mg11orbitclient167_GLOBAL__N__Volumes_Work_Projects_AC2_assassin_mac_main_projects_mac_engine_.._.._.._extern_orbit_mac_liborbit_src_mg_orbitclient_OrbitClientBase.cpp_00000000_2A92481D15GetErrLocStringEPKw
                --root __ZN8scimitar11OrbitClient10InitializeEv
                --root __ZN8scimitar11OrbitClient11GetInstanceEv
                --root __ZN8scimitar11OrbitClient12IsOrbitReadyEv
                --root __ZN8scimitar11OrbitClient5SetupEv
                --root __ZN8scimitar11OrbitClient13SaveGameWriteEmPhm
                --root __ZN8scimitar11OrbitClient12SaveGameReadEmRNS_8BigArrayIhNS_21DefaultAllocatorTraitEEE
                --root __ZN8scimitar11OrbitClient14SaveGameDeleteEm
                --override __ZN2mg11orbitclient13LibraryLoader15PrivLoadLibraryEv
                --out ${SAVES_GEN}
        DEPENDS scimitar-recomp "${AC2_BINARY}"
        COMMENT "Recompiling the game's save code (OrbitClient)"
        VERBATIM)
    add_executable(saves_test tests/saves/saves_test.cpp games/common/overrides.cpp ${SAVES_GEN})
    target_link_libraries(saves_test PRIVATE scimitar_platform)
    # Shims must win over the generated weak fallbacks.
    target_link_options(saves_test PRIVATE "LINKER:-force_load,$<TARGET_FILE:scimitar_platform>")
    set_source_files_properties(${SAVES_GEN} PROPERTIES COMPILE_OPTIONS "-O1")
    add_test(NAME saves COMMAND saves_test "${AC2_APP}")

    # Differential test: game functions chosen by scimitar-analyze (whole
    # closure translatable, only imports with shims) run recompiled and in
    # Unicorn with generated inputs; results must be identical.
    set(DIFFTEST_IMPORTS _memcpy _memmove _free)
    set(DIFFTEST_ROOTS ${CMAKE_BINARY_DIR}/gen/difftest_roots.txt)
    set(DIFFTEST_GEN ${CMAKE_BINARY_DIR}/gen/difftest_functions.cpp)
    set(DIFFTEST_ALLOW)
    foreach(imp ${DIFFTEST_IMPORTS})
        list(APPEND DIFFTEST_ALLOW --allow-import ${imp})
    endforeach()
    add_custom_command(
        OUTPUT ${DIFFTEST_ROOTS}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
        COMMAND scimitar-analyze "${AC2_BINARY}" --difftest-candidates --max 1500
                --max-closure 48 ${DIFFTEST_ALLOW} --out ${DIFFTEST_ROOTS}
        DEPENDS scimitar-analyze "${AC2_BINARY}"
        COMMENT "Choosing game functions for the differential test"
        VERBATIM)
    add_custom_command(
        OUTPUT ${DIFFTEST_GEN}
        COMMAND scimitar-recomp --image "${AC2_BINARY}" --roots-file ${DIFFTEST_ROOTS}
                --no-vtables --out ${DIFFTEST_GEN}
        DEPENDS scimitar-recomp ${DIFFTEST_ROOTS}
        COMMENT "Recompiling the differential test's functions"
        VERBATIM)
    add_executable(difftest tests/difftest/difftest.cpp ${DIFFTEST_GEN})
    target_include_directories(difftest PRIVATE recomp/src)
    target_link_libraries(difftest PRIVATE scimitar_platform recomp_core PkgConfig::UNICORN)
    set_source_files_properties(${DIFFTEST_GEN} PROPERTIES COMPILE_OPTIONS "-O1")
    add_test(NAME difftest COMMAND difftest "${AC2_APP}" ${DIFFTEST_ROOTS})
    # The native replacements of hot game math (games/ac2/native_math.cpp)
    # against the original x86 in Unicorn.
    add_executable(native_test tests/difftest/native_test.cpp games/ac2/native_math.cpp)
    target_link_libraries(native_test PRIVATE scimitar_runtime PkgConfig::UNICORN)
    add_test(NAME native_math COMMAND native_test "${AC2_APP}")
    # ACB's (games/acb/native_math.cpp).
    if(EXISTS "${CMAKE_SOURCE_DIR}/../steam_depot/48193_15/Assassin's Creed Brotherhood.app")
        add_executable(native_test_acb tests/difftest/native_test.cpp games/acb/native_math.cpp)
        target_compile_definitions(native_test_acb PRIVATE NATIVE_ACB)
        target_link_libraries(native_test_acb PRIVATE scimitar_runtime PkgConfig::UNICORN)
        add_test(NAME native_math_acb COMMAND native_test_acb
                 "${CMAKE_SOURCE_DIR}/../steam_depot/48193_15/Assassin's Creed Brotherhood.app")
    endif()
    if(APPLE)
        add_test(NAME gl_arb_ac2 COMMAND gl_arb_probe "${AC2_BINARY}")
        add_test(NAME arb_msl_ac2 COMMAND arb_msl_test --pipelines "${AC2_BINARY}")
        # The programs stored in the .forge archives (needs liblzo2).
        if(LZO2_LIBRARY AND LZO2_INCLUDE_DIR)
            add_executable(forge_shaders tests/gl/forge_shaders.cpp)
            target_include_directories(forge_shaders PRIVATE ${LZO2_INCLUDE_DIR})
            target_link_libraries(forge_shaders PRIVATE ${LZO2_LIBRARY})
            add_test(NAME forge_shaders COMMAND forge_shaders "${AC2_APP}"
                     ${CMAKE_BINARY_DIR}/forge_programs.txt)
            set_tests_properties(forge_shaders PROPERTIES FIXTURES_SETUP forge_programs)
            add_test(NAME gl_arb_forge COMMAND gl_arb_probe ${CMAKE_BINARY_DIR}/forge_programs.txt)
            set_tests_properties(gl_arb_forge PROPERTIES FIXTURES_REQUIRED forge_programs)
            add_test(NAME arb_msl_forge COMMAND arb_msl_test ${CMAKE_BINARY_DIR}/forge_programs.txt)
            set_tests_properties(arb_msl_forge PROPERTIES FIXTURES_REQUIRED forge_programs)
        endif()
    endif()

else()
    message(STATUS "AC2 not found at ${AC2_APP}: game-dependent tests skipped")
endif()

# The ports (opt in): every function of a game translated (about 1 GB of C++
# per game, ~2 minutes on 10 cores at -O1) and linked with the platform layer,
# the common entry point and shell (games/common) and the game's own sources.
option(SCIMITAR_FULL_IMAGE "Build the ports: translate and compile every function of each game" OFF)
set(SCIMITAR_GAMES "ac2;acb" CACHE STRING "The games SCIMITAR_FULL_IMAGE builds (those installed)")
set(ACB_APP "${CMAKE_SOURCE_DIR}/../steam_depot/48193_15/Assassin's Creed Brotherhood.app"
    CACHE PATH "Local Assassin's Creed Brotherhood.app (Mac)")
# Optimization of the generated game code: the render thread spends most of
# its time there, so it pays to optimize, at the cost of build time.
set(SCIMITAR_GEN_OPT "-O2" CACHE STRING "Optimization flags for the translated game code")

# The Mac shell functions games/common/shell.mm replaces in every game, and
# the other common overrides (games/common/overrides.cpp): io_kit::Device's
# vendor, product and name, for the virtual game pad, and the platform name
# the UI gets, for its button art; and (games/common/intro.cpp) the start of
# the videos before the title, which can be skipped.
set(SCIMITAR_SHELL_OVERRIDES)
foreach(fn _MacApplicationInit _MacCreateWindow _MacInitializeOpenGL _MacSetContextOpenGL
           _MacUpdateOpenGL _MacVSyncOpenGL _MacApplicationLoop _MacGetMouseState _MacGetDisplayID
           __ZNK6io_kit6Device8vendorIdEv __ZNK6io_kit6Device9productIdEv __ZNK6io_kit6Device7productEv
           __ZN8scimitar15GetPlatformNameENS_8PlatformE __ZN8scimitar19StartupVideoManager5StartEv)
    list(APPEND SCIMITAR_SHELL_OVERRIDES ${fn})
endforeach()

# The launcher (games/common/launcher): SwiftUI, compiled once to an object
# that every port links. Its C entry points are in launcher.h.
set(SCIMITAR_LAUNCHER_OBJECT ${CMAKE_BINARY_DIR}/launcher/Launcher.o)
add_custom_command(
    OUTPUT ${SCIMITAR_LAUNCHER_OBJECT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/launcher
    COMMAND xcrun swiftc -parse-as-library -emit-object -swift-version 5 -O -module-name ScimitarLauncher
            -target arm64-apple-macos14.0 ${CMAKE_SOURCE_DIR}/games/common/launcher/Launcher.swift
            -o ${SCIMITAR_LAUNCHER_OBJECT}
    DEPENDS ${CMAKE_SOURCE_DIR}/games/common/launcher/Launcher.swift
    COMMENT "Compiling the launcher"
    VERBATIM)
add_custom_target(scimitar_launcher DEPENDS ${SCIMITAR_LAUNCHER_OBJECT})
execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE SCIMITAR_MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)

# scimitar_add_app(ID GAME_LOCATION APP_NAME APP_TITLE [GAME_EXE]): the
# launcher in port ID (games/common/launcher/launch.cpp; it shows when the port
# starts from its app), GAME_LOCATION (where the build found the game: its
# .app or folder) as the launcher's default, and APP_NAME.app made after each
# link (make_app.cmake). Its icon comes from the installed game: an .icns in
# GAME_LOCATION's Resources, else the Windows executable GAME_EXE's own.
function(scimitar_add_app id location app_name app_title)
    set(game_exe "${ARGN}")
    target_sources(${id} PRIVATE games/common/launcher/launch.cpp)
    target_include_directories(${id} PRIVATE games/common)
    target_compile_definitions(${id} PRIVATE SCIMITAR_LAUNCHER "SCIMITAR_LAUNCHER_DEFAULT_APP=\"${location}\"")
    add_dependencies(${id} scimitar_launcher)
    set_property(TARGET ${id} APPEND PROPERTY LINK_DEPENDS ${SCIMITAR_LAUNCHER_OBJECT})
    target_link_libraries(${id} PRIVATE ${SCIMITAR_LAUNCHER_OBJECT} "-framework SwiftUI"
        "-L${SCIMITAR_MACOS_SDK}/usr/lib/swift" "-L/usr/lib/swift")
    set(icon_args)
    if(game_exe)
        add_dependencies(${id} scimitar-pe-icon)
        set(icon_args -DGAME_EXE=${game_exe} -DPE_ICON=$<TARGET_FILE:scimitar-pe-icon>)
    endif()
    if(app_name)
        add_custom_command(TARGET ${id} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${id}> -DAPP=${CMAKE_BINARY_DIR}/${app_name}.app
                    -DID=${id} -DNAME=${app_name} -DTITLE=${app_title} -DGAME_APP=${location} ${icon_args}
                    -P ${CMAKE_SOURCE_DIR}/games/common/launcher/make_app.cmake
            COMMENT "Making ${app_name}.app"
            VERBATIM)
    endif()
endfunction()

# scimitar_add_game(ID BINARY APP_NAME name APP_TITLE title OVERRIDES symbol...
# SOURCES file...): the port `ID` of the game executable BINARY: ID_full (the
# translated image, in gen/full/ID_N.cpp), the executable ID, and the app
# APP_NAME.app (the same executable with the launcher, its Info.plist and the
# installed game's icon) next to it.
function(scimitar_add_game id binary)
    cmake_parse_arguments(G "" "APP_NAME;APP_TITLE" "OVERRIDES;SOURCES" ${ARGN})
    set(parts 64)
    set(overrides)
    foreach(fn ${SCIMITAR_SHELL_OVERRIDES} ${G_OVERRIDES})
        list(APPEND overrides --override ${fn})
    endforeach()
    set(gen)
    math(EXPR last "${parts} - 1")
    foreach(k RANGE ${last})
        list(APPEND gen ${CMAKE_BINARY_DIR}/gen/full/${id}_${k}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${gen}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen/full
        COMMAND scimitar-recomp --image "${binary}" --all --parts ${parts} --weak-imports ${overrides}
                --out ${CMAKE_BINARY_DIR}/gen/full/${id}.cpp
        DEPENDS scimitar-recomp "${binary}"
        COMMENT "Translating every function of the local ${id} binary"
        VERBATIM)
    add_library(${id}_full STATIC ${gen})
    target_link_libraries(${id}_full PUBLIC scimitar_runtime)
    separate_arguments(opt UNIX_COMMAND "${SCIMITAR_GEN_OPT}")
    set_source_files_properties(${gen} PROPERTIES COMPILE_OPTIONS "${opt}")

    set(objc games/common/shell.mm games/common/shell_common.mm)
    add_executable(${id} games/common/main.cpp games/common/overrides.cpp games/common/intro.cpp
                       games/common/graphics_menu.cpp ${objc} ${G_SOURCES})
    target_include_directories(${id} PRIVATE games/common)
    set_source_files_properties(${objc} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    find_library(COCOA_FW Cocoa REQUIRED)
    find_library(GAMECONTROLLER_FW GameController REQUIRED)
    target_link_libraries(${id} PRIVATE scimitar_platform ${COCOA_FW} ${GAMECONTROLLER_FW}
        "LINKER:-force_load,$<TARGET_FILE:${id}_full>"
        "LINKER:-force_load,$<TARGET_FILE:scimitar_platform>")
    add_dependencies(${id} ${id}_full)
    # The game's .app (the binary's), where the launcher looks first.
    get_filename_component(game_app "${binary}" DIRECTORY)
    get_filename_component(game_app "${game_app}/../.." ABSOLUTE)
    scimitar_add_app(${id} "${game_app}" "${G_APP_NAME}" "${G_APP_TITLE}")
endfunction()

if(SCIMITAR_FULL_IMAGE AND APPLE)
    if("ac2" IN_LIST SCIMITAR_GAMES AND EXISTS "${AC2_BINARY}")
        # AC2 links the Orbit client statically: its library loader installs the
        # local Orbit. Hot math of the render thread, native (native_math.cpp,
        # tested by native_test; shader constants, native_gfx.cpp). The view's aspect fills any screen (view.cpp),
        # the HUD's movies keep to its edges (hud.cpp), and text and voices
        # can be in different languages (language.cpp).
        scimitar_add_game(ac2 "${AC2_BINARY}" APP_NAME AC2 APP_TITLE "Assassin's Creed II"
            OVERRIDES _MacScheduleSwitchMode _MacSwitchModeIfNeeded
                      __ZN2mg11orbitclient13LibraryLoader15PrivLoadLibraryEv
                      __ZN8scimitar4View18UpdateViewportSizeEv __ZN8scimitar8FireItem6RenderEv
                      __ZN8scimitar18CullPlaneAABBoxResERKN4Gear7Vector4IfEES4_S4_
                      __ZN4Gear8Matrix44IfEmLERKS1_ __ZNK4Gear11Quaternion4IfE8ToMatrixERNS_8Matrix44IfEE
                      __ZN8scimitar15IsConstantEqualIN4Gear7Vector4IfEEEEbRKT_S6_
                      __ZN8scimitar6IColor16ConvertFromFloatERKN4Gear7Vector4IfEE
                      __ZN8scimitar23update_shader_constantsIN4Gear7Vector4IfEEXadL_ZNS_9SetVSCEnvEjPKS3_jEEEEvjPT_PKS6_j
                      __ZN8scimitar23update_shader_constantsIN4Gear7Vector4IfEEXadL_ZNS_9SetPSCEnvEjPKS3_jEEEEvjPT_PKS6_j
                      __ZN8scimitar23SelectInstalledLanguageEv
                      __ZN8scimitar10GameConfig27ValidateAndSetSoundLanguageENS_8LanguageE
            SOURCES games/ac2/game.cpp games/ac2/native_math.cpp games/ac2/native_gfx.cpp
                    games/common/native_constants.cpp games/ac2/view.cpp
                    games/ac2/hud.cpp games/common/hud.cpp games/ac2/language.cpp)
    endif()
    set(ACB_BINARY "${ACB_APP}/Contents/MacOS/scimitar_sf")
    if("acb" IN_LIST SCIMITAR_GAMES AND EXISTS "${ACB_BINARY}")
        # Brotherhood switches modes at once (MacSwitchModeOpenGL), locks the
        # view's focus around drawing, and launches its multiplayer executable,
        # the launcher's updater and other bundles (none are started here).
        # The view's aspect fills any screen, the icons over people follow
        # (view.cpp), the HUD's movies keep to its edges (hud.cpp), and the
        # text and voice languages can be chosen outside the game and the options
        # offer every installed one (language.cpp), and the menus' button art
        # follows the prompts chosen (GetSelectedPadType, overrides.cpp). Hot math
        # of the render thread, native (games/acb/native_math.cpp, tested by
        # native_test_acb).
        scimitar_add_game(acb "${ACB_BINARY}" APP_NAME ACB APP_TITLE "Assassin's Creed Brotherhood"
            OVERRIDES _MacSwitchModeOpenGL _MacLockFocusIfCanDraw _MacUnlockFocus _MacLaunchExecutable
                      _MacCheckUpdaterLaunch _MacLaunchBundle
                      __ZNK8scimitar4View23UpdateViewportSizeConstERNS_4RectERNS_14DisplayOptions11AspectRatioE
                      __ZN8scimitar3ac27UIUtils17OffsetToLowerLeftERN4Gear7Vector2IfEE
                      __ZN8scimitar8FireItem6RenderEv
                      __ZN8scimitar17LocalizationUtils26ValidateAndSetTextLanguageENS_8LanguageE
                      __ZN8scimitar17LocalizationUtils30ValidateAndSetSubtitleLanguageENS_8LanguageE
                      __ZN8scimitar17LocalizationUtils27ValidateAndSetSoundLanguageENS_8LanguageE
                      __ZN8scimitar17LocalizationUtils22ReadSupportedLanguagesEv
                      __ZN8scimitar16PadManagerNative18GetSelectedPadTypeEv
                      __ZN8scimitar18CullPlaneAABBoxResERKN4Gear7Vector4IfEES4_S4_
                      __ZN4Gear8Matrix44IfEmLERKS1_ __ZNK4Gear11Quaternion4IfE8ToMatrixERNS_8Matrix44IfEE
                      __ZN8scimitar15IsConstantEqualIN4Gear7Vector4IfEEEEbRKT_S6_
                      __ZN8scimitar23update_shader_constantsIN4Gear7Vector4IfEEXadL_ZNS_9SetVSCEnvEjPKS3_jEEEEvjPT_PKS6_j
                      __ZN8scimitar23update_shader_constantsIN4Gear7Vector4IfEEXadL_ZNS_9SetPSCEnvEjPKS3_jEEEEvjPT_PKS6_j
                      __ZN8scimitar6IColor16ConvertFromFloatERKN4Gear7Vector4IfEE
            SOURCES games/acb/game.cpp games/acb/view.cpp games/acb/hud.cpp games/common/hud.cpp
                    games/acb/language.cpp games/acb/native_math.cpp games/acb/native_gfx.cpp
                    games/common/native_constants.cpp)
    endif()
endif()

# The Windows releases (docs/AC1_WINDOWS.md, docs/ACR_WINDOWS.md): each
# executable translated whole like the Mac games, and linked with the Windows
# platform layer (platform/win32) instead of the Mac one.
set(AC1_EXE "${CMAKE_SOURCE_DIR}/../windows-releases/Ubisoft Launcher/Assassin's Creed 1/AssassinsCreed_Dx9.exe"
    CACHE FILEPATH "Local Assassin's Creed (Windows) AssassinsCreed_Dx9.exe")
set(ACR_EXE "${CMAKE_SOURCE_DIR}/../windows-releases/Steam/Assassin's Creed Revelations/ACRSP.exe"
    CACHE FILEPATH "Local Assassin's Creed Revelations (Windows) ACRSP.exe")
set(POP_EXE "${CMAKE_SOURCE_DIR}/../steam_depot/Prince of Persia.app/Contents/Resources/transgaming/c_drive/Program Files/Ubisoft/Prince of Persia/Prince of Persia.exe"
    CACHE FILEPATH "Local Prince of Persia (2008, Windows, from the Mac Cider release) Prince of Persia.exe")
set(SCIMITAR_AC1_GEN_OPT "-O1 -gline-tables-only" CACHE STRING "Optimization flags for AC1's translated code (line tables: atos names the generated line, whose comment is the guest instruction)")
set(SCIMITAR_ACR_GEN_OPT "-O1 -gline-tables-only" CACHE STRING "Optimization flags for Revelations' translated code")
set(SCIMITAR_POP_GEN_OPT "-O1 -gline-tables-only" CACHE STRING "Optimization flags for Prince of Persia's translated code")

# scimitar_add_windows_game(ID EXE PARTS OPT MAIN APP_NAME name APP_TITLE title
# [OVERRIDES 0xADDR...] [SOURCES file...]): the port ID of the Windows
# executable EXE, its code in PARTS generated files compiled with OPT, its
# entry point in MAIN; functions replaced by hand-written ones
# (scimitar::overrides::ov_sub_XXXXXXXX) and the files that hold them (and
# games/ID/game.cpp, its game::Info). Its app APP_NAME.app, with the launcher,
# is made next to it, like the Mac ports'.
function(scimitar_add_windows_game id exe parts opt main)
    cmake_parse_arguments(G "" "APP_NAME;APP_TITLE" "OVERRIDES;SOURCES" ${ARGN})
    set(overrides)
    foreach(fn ${G_OVERRIDES})
        list(APPEND overrides --override ${fn})
    endforeach()
    set(gen)
    math(EXPR last "${parts} - 1")
    foreach(k RANGE ${last})
        list(APPEND gen ${CMAKE_BINARY_DIR}/gen/full/${id}_${k}.cpp)
    endforeach()
    # The parts are byproducts: the translator leaves unchanged ones untouched,
    # so only parts whose code changed are compiled again.
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/gen/full/${id}.stamp
        BYPRODUCTS ${gen}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen/full
        COMMAND scimitar-recomp --image "${exe}" --all --parts ${parts} --weak-imports ${overrides}
                --out ${CMAKE_BINARY_DIR}/gen/full/${id}.cpp
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/gen/full/${id}.stamp
        DEPENDS scimitar-recomp "${exe}"
        COMMENT "Translating every function of the local ${id} executable"
        VERBATIM)
    add_custom_target(${id}_gen DEPENDS ${CMAKE_BINARY_DIR}/gen/full/${id}.stamp)
    add_library(${id}_full STATIC ${gen})
    add_dependencies(${id}_full ${id}_gen)
    target_link_libraries(${id}_full PUBLIC scimitar_runtime)
    separate_arguments(o UNIX_COMMAND "${opt}")
    set_source_files_properties(${gen} PROPERTIES COMPILE_OPTIONS "${o}")
    add_executable(${id} ${main} games/${id}/game.cpp ${G_SOURCES})
    target_link_libraries(${id} PRIVATE scimitar_win32 ${COCOA_FW} ${QUARTZCORE_FW}
        "LINKER:-force_load,$<TARGET_FILE:${id}_full>"
        "LINKER:-force_load,$<TARGET_FILE:scimitar_win32>")
    add_dependencies(${id} ${id}_full)
    # -force_load hides the libraries from CMake's link dependencies.
    set_target_properties(${id} PROPERTIES LINK_DEPENDS "$<TARGET_FILE:${id}_full>;$<TARGET_FILE:scimitar_win32>")
    # The game's location, where the launcher looks first: a Mac release's
    # .app holding the executable (Prince of Persia's Cider bundle), else the
    # executable's folder.
    if(exe MATCHES "^(.*\\.app)/")
        set(location "${CMAKE_MATCH_1}")
    else()
        get_filename_component(location "${exe}" DIRECTORY)
    endif()
    get_filename_component(location "${location}" ABSOLUTE)
    scimitar_add_app(${id} "${location}" "${G_APP_NAME}" "${G_APP_TITLE}" "${exe}")
endfunction()

set(SCIMITAR_WINDOWS_GAMES)
if(SCIMITAR_FULL_IMAGE AND APPLE)
    if("ac1" IN_LIST SCIMITAR_GAMES AND EXISTS "${AC1_EXE}")
        list(APPEND SCIMITAR_WINDOWS_GAMES ac1)
    endif()
    if("acr" IN_LIST SCIMITAR_GAMES AND EXISTS "${ACR_EXE}")
        list(APPEND SCIMITAR_WINDOWS_GAMES acr)
    endif()
    if("pop" IN_LIST SCIMITAR_GAMES AND EXISTS "${POP_EXE}")
        list(APPEND SCIMITAR_WINDOWS_GAMES pop)
    endif()
endif()
if(SCIMITAR_WINDOWS_GAMES)
    file(GLOB WIN32_SOURCES platform/win32/*.cpp platform/win32/*.mm)
    add_library(scimitar_win32 STATIC ${WIN32_SOURCES})
    target_include_directories(scimitar_win32 PUBLIC platform/win32 platform/include)
    target_link_libraries(scimitar_win32 PUBLIC scimitar_runtime scimitar_platform)
    foreach(src ${WIN32_SOURCES})
        if(src MATCHES "\\.mm$")
            set_source_files_properties(${src} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        endif()
    endforeach()
    find_library(COCOA_FW Cocoa REQUIRED)
    find_library(QUARTZCORE_FW QuartzCore REQUIRED)
    # Their apps' icons: the executable's own (tools/pe_icon.cpp), at build time.
    add_executable(scimitar-pe-icon tools/pe_icon.cpp)
    if("ac1" IN_LIST SCIMITAR_WINDOWS_GAMES)
        scimitar_add_windows_game(ac1 "${AC1_EXE}" 48 "${SCIMITAR_AC1_GEN_OPT}" games/ac1/main.cpp
            APP_NAME AC1 APP_TITLE "Assassin's Creed")
    endif()
    # Revelations creates its window on an engine thread: its entry point runs
    # the guest main thread on a host thread of its own (games/acr/main.cpp).
    if("acr" IN_LIST SCIMITAR_WINDOWS_GAMES)
        scimitar_add_windows_game(acr "${ACR_EXE}" 64 "${SCIMITAR_ACR_GEN_OPT}" games/acr/main.cpp
            APP_NAME ACR APP_TITLE "Assassin's Creed Revelations")
    endif()
    # Prince of Persia (2008, docs/POP_WINDOWS.md): the Windows executable of
    # the Mac (Cider) release, run like AC1.
    if("pop" IN_LIST SCIMITAR_WINDOWS_GAMES)
        scimitar_add_windows_game(pop "${POP_EXE}" 32 "${SCIMITAR_POP_GEN_OPT}" games/pop/main.cpp
            APP_NAME PoP APP_TITLE "Prince of Persia" OVERRIDES 0x51f820 0x51f840)
    endif()
endif()

# AC2 with the Windows release's data: the records that differ converted as
# they load (games/ac2/pc_data.cpp, docs/PC_DATA.md).
if(TARGET ac2)
    target_sources(ac2 PRIVATE games/ac2/pc_data.cpp games/ac2/color_filter.cpp)
endif()

# Regenerates tests/lzo/vectors from liblzo2 (optional; vectors are committed).
if(LZO2_LIBRARY AND LZO2_INCLUDE_DIR)
    add_executable(lzo_gen_vectors tests/lzo/gen_vectors.c)
    target_include_directories(lzo_gen_vectors PRIVATE ${LZO2_INCLUDE_DIR})
    target_link_libraries(lzo_gen_vectors PRIVATE ${LZO2_LIBRARY})
endif()
