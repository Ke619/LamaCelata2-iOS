# The AC2 and Brotherhood ports for iOS and the iOS Simulator (included by the
# top CMakeLists when CMAKE_SYSTEM_NAME is iOS). The translator runs on the Mac:
# the game's C++ comes from a Mac build with SCIMITAR_FULL_IMAGE=ON
# (SCIMITAR_GEN_DIR). One game per build directory (SCIMITAR_IOS_GAME). See
# games/ac2/ios/README.md.

set(SCIMITAR_IOS_GAME "ac2" CACHE STRING "The game this build makes: ac2 or acb")
set_property(CACHE SCIMITAR_IOS_GAME PROPERTY STRINGS ac2 acb)
set(IOS_GAME ${SCIMITAR_IOS_GAME})
string(TOUPPER ${IOS_GAME} IOS_APP_NAME)
# Each game's own sources (games/<id>), as in its Mac target (CMakeLists.txt,
# scimitar_add_game), and the Mac game's icon, the source of the app's.
if(IOS_GAME STREQUAL "ac2")
    set(IOS_GAME_SOURCES games/ac2/game.cpp games/ac2/native_math.cpp games/ac2/native_gfx.cpp
        games/common/native_constants.cpp games/ac2/view.cpp games/ac2/hud.cpp games/common/hud.cpp
        games/ac2/language.cpp games/ac2/pc_data.cpp games/ac2/color_filter.cpp)
    set(IOS_ICON_DEFAULT "${CMAKE_SOURCE_DIR}/../steam_depot/33362_5/Assassin's Creed 2.app/Contents/Resources/AC2.icns")
elseif(IOS_GAME STREQUAL "acb")
    set(IOS_GAME_SOURCES games/acb/game.cpp games/acb/view.cpp games/acb/hud.cpp games/common/hud.cpp
        games/acb/language.cpp games/acb/native_math.cpp games/acb/native_gfx.cpp
        games/common/native_constants.cpp)
    set(IOS_ICON_DEFAULT "${CMAKE_SOURCE_DIR}/../steam_depot/48193_15/Assassin's Creed Brotherhood.app/Contents/Resources/ACB.icns")
else()
    message(FATAL_ERROR "SCIMITAR_IOS_GAME: no iOS build for '${IOS_GAME}' (ac2 or acb)")
endif()

set(SCIMITAR_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen/full"
    CACHE PATH "The translated game (gen/full of a Mac build with SCIMITAR_FULL_IMAGE=ON)")
file(GLOB IOS_GEN_SOURCES "${SCIMITAR_GEN_DIR}/${IOS_GAME}_*.cpp")
if(NOT IOS_GEN_SOURCES)
    message(FATAL_ERROR "No translated ${IOS_GAME} in ${SCIMITAR_GEN_DIR}: build the Mac port first "
                        "(SCIMITAR_FULL_IMAGE=ON) or set SCIMITAR_GEN_DIR")
endif()

# AC2_BUNDLE_ID, ACB_BUNDLE_ID. The signing options below keep their AC2_
# names (the first port's) and serve either game.
set(${IOS_APP_NAME}_BUNDLE_ID "github.ke619.${IOS_GAME}" CACHE STRING "The app's bundle identifier")
set(IOS_BUNDLE_ID ${${IOS_APP_NAME}_BUNDLE_ID})

add_library(scimitar_macho STATIC recomp/src/macho.cpp recomp/src/pe.cpp)
target_include_directories(scimitar_macho PUBLIC recomp/src)

add_library(scimitar_runtime STATIC ${SCIMITAR_RUNTIME_SOURCES})
target_include_directories(scimitar_runtime PUBLIC runtime/include)
target_link_libraries(scimitar_runtime PUBLIC scimitar_macho)

add_library(scimitar_arb_msl STATIC platform/src/arb_msl.cpp)
target_include_directories(scimitar_arb_msl PUBLIC platform/include)

# No desktop OpenGL: the GL imports all go to the Metal backend (opengl_metal.cpp).
add_library(scimitar_platform STATIC ${SCIMITAR_PLATFORM_SOURCES} ${SCIMITAR_METAL_SOURCES}
            platform/src/opengl_metal.cpp)
target_include_directories(scimitar_platform PUBLIC platform/include)
set_source_files_properties(${SCIMITAR_METAL_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(scimitar_platform PUBLIC scimitar_runtime scimitar_arb_msl
    "-framework CoreFoundation" "-framework Foundation" "-framework CoreGraphics"
    "-framework AudioToolbox" "-framework Metal" "-framework QuartzCore")

# The Bink videos (platform/src/bink.cpp) through FFmpeg's bink decoders, built
# for iOS by tools/build_ffmpeg_ios.sh (static libraries, outside the repo).
# Without them the game skips its videos.
if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(AC2_FFMPEG_SDK iphonesimulator)
else()
    set(AC2_FFMPEG_SDK iphoneos)
endif()
set(SCIMITAR_FFMPEG_IOS "$ENV{HOME}/Library/Developer/scimitar/ffmpeg-${AC2_FFMPEG_SDK}"
    CACHE PATH "FFmpeg's Bink decoders for this SDK (tools/build_ffmpeg_ios.sh)")
if(EXISTS "${SCIMITAR_FFMPEG_IOS}/lib/libavcodec.a")
    target_include_directories(scimitar_platform PRIVATE "${SCIMITAR_FFMPEG_IOS}/include")
    target_compile_definitions(scimitar_platform PRIVATE SCIMITAR_HAVE_FFMPEG)
    foreach(lib avformat avcodec swscale swresample avutil)
        target_link_libraries(scimitar_platform PUBLIC "${SCIMITAR_FFMPEG_IOS}/lib/lib${lib}.a")
    endforeach()
    message(STATUS "Bink videos: FFmpeg in ${SCIMITAR_FFMPEG_IOS}")
else()
    message(STATUS "Bink videos: no FFmpeg in ${SCIMITAR_FFMPEG_IOS} (tools/build_ffmpeg_ios.sh "
                   "${AC2_FFMPEG_SDK}); the game skips its videos")
endif()

add_library(${IOS_GAME}_full STATIC ${IOS_GEN_SOURCES})
target_link_libraries(${IOS_GAME}_full PUBLIC scimitar_runtime)
set(SCIMITAR_GEN_OPT "-O2" CACHE STRING "Optimization flags for the translated game code")
separate_arguments(SCIMITAR_GEN_OPT_LIST UNIX_COMMAND "${SCIMITAR_GEN_OPT}")
set_source_files_properties(${IOS_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${SCIMITAR_GEN_OPT_LIST}")

# The launcher (games/common/launcher/Launcher.swift): SwiftUI, shown before
# the game starts.
if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(AC2_SWIFT_SDK iphonesimulator)
    set(AC2_SWIFT_TARGET arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}-simulator)
else()
    set(AC2_SWIFT_SDK iphoneos)
    set(AC2_SWIFT_TARGET arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()
set(AC2_LAUNCHER_OBJECT ${CMAKE_BINARY_DIR}/launcher/Launcher.o)
add_custom_command(
    OUTPUT ${AC2_LAUNCHER_OBJECT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/launcher
    COMMAND xcrun --sdk ${AC2_SWIFT_SDK} swiftc -parse-as-library -emit-object -wmo -swift-version 5 -O
            -module-name ScimitarLauncher -target ${AC2_SWIFT_TARGET}
            ${CMAKE_SOURCE_DIR}/games/common/launcher/Launcher.swift
            ${CMAKE_SOURCE_DIR}/games/common/launcher/LauncherPhoneUI.swift -o ${AC2_LAUNCHER_OBJECT}
    DEPENDS ${CMAKE_SOURCE_DIR}/games/common/launcher/Launcher.swift
            ${CMAKE_SOURCE_DIR}/games/common/launcher/LauncherPhoneUI.swift
    COMMENT "Compiling the launcher"
    VERBATIM)
execute_process(COMMAND xcrun --sdk ${AC2_SWIFT_SDK} --show-sdk-path OUTPUT_VARIABLE AC2_SWIFT_SDK_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE)

add_executable(${IOS_GAME} MACOSX_BUNDLE ${AC2_LAUNCHER_OBJECT} games/common/main.cpp games/common/overrides.cpp
               games/common/intro.cpp games/common/graphics_menu.cpp ${IOS_GAME_SOURCES}
               games/common/shell_ios.mm games/common/shell_common.mm games/common/touchpad_ios.mm)
target_include_directories(${IOS_GAME} PRIVATE games/common)
target_compile_definitions(${IOS_GAME} PRIVATE SCIMITAR_LAUNCHER)
set_source_files_properties(games/common/shell_ios.mm games/common/shell_common.mm games/common/touchpad_ios.mm
                            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(AC2_PLATFORM iPhoneSimulator)
else()
    set(AC2_PLATFORM iPhoneOS)
endif()

# The app icon, made at build time from the local game's Mac icon (game art:
# never in the repo). Without it the app has no icon.
# AC2_ICON_SOURCE, ACB_ICON_SOURCE.
set(${IOS_APP_NAME}_ICON_SOURCE "${IOS_ICON_DEFAULT}"
    CACHE FILEPATH "The Mac game's icon (.icns), the source of the iOS app icon")
set(IOS_ICON_SOURCE "${${IOS_APP_NAME}_ICON_SOURCE}")
set(AC2_ICON_PLIST "")
if(EXISTS "${IOS_ICON_SOURCE}")
    set(AC2_ICON_DIR ${CMAKE_BINARY_DIR}/icon)
    set(AC2_ICON_SET ${AC2_ICON_DIR}/Assets.xcassets/AppIcon.appiconset)
    file(WRITE ${AC2_ICON_DIR}/Assets.xcassets/Contents.json "{\"info\":{\"author\":\"xcode\",\"version\":1}}\n")
    file(WRITE ${AC2_ICON_SET}/Contents.json
         "{\"images\":[{\"filename\":\"icon.png\",\"idiom\":\"universal\",\"platform\":\"ios\",\"size\":\"1024x1024\"}],"
         "\"info\":{\"author\":\"xcode\",\"version\":1}}\n")
    add_custom_command(OUTPUT ${AC2_ICON_SET}/icon.png
        COMMAND xcrun --sdk macosx swift ${CMAKE_SOURCE_DIR}/games/ac2/ios/make_icon.swift
                "${IOS_ICON_SOURCE}" ${AC2_ICON_SET}/icon.png
        DEPENDS "${IOS_ICON_SOURCE}" ${CMAKE_SOURCE_DIR}/games/ac2/ios/make_icon.swift
        COMMENT "Making the app icon"
        VERBATIM)
    set(AC2_ICON_FILES ${AC2_ICON_DIR}/out/Assets.car ${AC2_ICON_DIR}/out/AppIcon60x60@2x.png
                       ${AC2_ICON_DIR}/out/AppIcon76x76@2x~ipad.png)
    string(TOLOWER ${AC2_PLATFORM} AC2_ACTOOL_PLATFORM)
    add_custom_command(OUTPUT ${AC2_ICON_FILES}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AC2_ICON_DIR}/out
        COMMAND xcrun actool ${AC2_ICON_DIR}/Assets.xcassets --compile ${AC2_ICON_DIR}/out
                --platform ${AC2_ACTOOL_PLATFORM} --minimum-deployment-target ${CMAKE_OSX_DEPLOYMENT_TARGET}
                --app-icon AppIcon --target-device iphone --target-device ipad
                --output-partial-info-plist ${AC2_ICON_DIR}/partial.plist
                --output-format human-readable-text
        DEPENDS ${AC2_ICON_SET}/icon.png ${AC2_ICON_SET}/Contents.json
        COMMENT "Compiling the app icon"
        VERBATIM)
    target_sources(${IOS_GAME} PRIVATE ${AC2_ICON_FILES})
    set_source_files_properties(${AC2_ICON_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # The keys actool writes to partial.plist.
    set(AC2_ICON_PLIST [=[
    <key>CFBundleIcons</key>
    <dict>
        <key>CFBundlePrimaryIcon</key>
        <dict>
            <key>CFBundleIconFiles</key>
            <array>
                <string>AppIcon60x60</string>
            </array>
            <key>CFBundleIconName</key>
            <string>AppIcon</string>
        </dict>
    </dict>
    <key>CFBundleIcons~ipad</key>
    <dict>
        <key>CFBundlePrimaryIcon</key>
        <dict>
            <key>CFBundleIconFiles</key>
            <array>
                <string>AppIcon60x60</string>
                <string>AppIcon76x76</string>
            </array>
            <key>CFBundleIconName</key>
            <string>AppIcon</string>
        </dict>
    </dict>]=])
else()
    message(STATUS "No ${IOS_ICON_SOURCE}: the app has no icon")
endif()
configure_file(games/ac2/ios/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
set_target_properties(${IOS_GAME} PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist
                                           OUTPUT_NAME ${IOS_GAME})
target_link_libraries(${IOS_GAME} PRIVATE scimitar_platform
    "-framework UIKit" "-framework GameController" "-framework AVFAudio" "-framework SwiftUI"
    "-L${AC2_SWIFT_SDK_PATH}/usr/lib/swift" "-L/usr/lib/swift"
    "LINKER:-force_load,$<TARGET_FILE:${IOS_GAME}_full>"
    "LINKER:-force_load,$<TARGET_FILE:scimitar_platform>")
add_dependencies(${IOS_GAME} ${IOS_GAME}_full)

# Signing: ad hoc for the Simulator; a device build takes an identity and a
# provisioning profile (AC2_SIGN_IDENTITY, AC2_PROFILE) and entitlements
# (ac2.entitlements). AC2_MEMORY_ENTITLEMENTS adds the two kernel ones, which
# only paid accounts get. The iPad Air M3 runs the game without them (a 2 GB
# guest block, guest_memory.h; at most about 1.3 GB in use in Florence, where
# iOS allows about 5 GB); devices with less memory may need them.
option(AC2_MEMORY_ENTITLEMENTS "Sign with extended virtual addressing and the increased memory limit (paid accounts)" OFF)
set(AC2_MEMORY_KEYS "")
if(AC2_MEMORY_ENTITLEMENTS)
    string(CONCAT AC2_MEMORY_KEYS
        "    <key>com.apple.developer.kernel.extended-virtual-addressing</key>\n    <true/>\n"
        "    <key>com.apple.developer.kernel.increased-memory-limit</key>\n    <true/>")
endif()
set(AC2_SIGN_IDENTITY "-" CACHE STRING "codesign identity (- for ad hoc, the Simulator)")
set(AC2_PROFILE "" CACHE FILEPATH "Provisioning profile (.mobileprovision) for a device build")
set(AC2_TEAM_ID "" CACHE STRING "Developer team for a device build: fills in ac2.entitlements.in")
set(AC2_ENTITLEMENTS "" CACHE FILEPATH "Entitlements for a device build (default: ac2.entitlements.in with AC2_TEAM_ID)")
set(AC2_SIGN_ARGS --force --sign "${AC2_SIGN_IDENTITY}")
if(AC2_TEAM_ID AND NOT AC2_ENTITLEMENTS)
    configure_file(games/ac2/ios/ac2.entitlements.in ${CMAKE_BINARY_DIR}/ac2.entitlements @ONLY)
    list(APPEND AC2_SIGN_ARGS --entitlements "${CMAKE_BINARY_DIR}/ac2.entitlements")
elseif(AC2_ENTITLEMENTS)
    list(APPEND AC2_SIGN_ARGS --entitlements "${AC2_ENTITLEMENTS}")
endif()
add_custom_command(TARGET ${IOS_GAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:${AC2_PROFILE}>,copy,true>
            "${AC2_PROFILE}" "$<TARGET_BUNDLE_DIR:${IOS_GAME}>/embedded.mobileprovision"
    COMMAND codesign ${AC2_SIGN_ARGS} "$<TARGET_BUNDLE_DIR:${IOS_GAME}>"
    COMMENT "Signing ${IOS_GAME}.app"
    VERBATIM)
