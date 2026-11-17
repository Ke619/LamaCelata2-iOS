# Makes a port's .app (cmake -P, after the port links; CMakeLists.txt,
# scimitar_add_app): the executable (an APFS clone, so no extra space), an
# Info.plist, the installed game's icon if there is one (game art: copied at
# build time, never in the repo), and an ad hoc signature.
#
#   -DEXE=build/ac2 -DAPP=build/AC2.app -DID=ac2 -DNAME=AC2
#   -DTITLE="Assassin's Creed II" -DGAME_APP=<the installed game's .app or folder>
#   [-DGAME_EXE=<a Windows port's .exe> -DPE_ICON=<tools/pe_icon.cpp's tool>]
#
# The icon: the first .icns in GAME_APP's Resources (a Mac release's), else
# GAME_EXE's own icon (its resources' first icon group, as an .ico, converted
# with sips).

foreach(v EXE APP ID NAME TITLE)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "make_app.cmake: ${v} not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${APP}")
file(MAKE_DIRECTORY "${APP}/Contents/MacOS" "${APP}/Contents/Resources")
execute_process(COMMAND cp -c "${EXE}" "${APP}/Contents/MacOS/${ID}" RESULT_VARIABLE cloned ERROR_QUIET)
if(NOT cloned EQUAL 0)
    file(COPY_FILE "${EXE}" "${APP}/Contents/MacOS/${ID}")
endif()

set(ICON_KEYS "")
if(GAME_APP)
    file(GLOB icons "${GAME_APP}/Contents/Resources/*.icns")
    if(icons)
        list(GET icons 0 icon)
        file(COPY_FILE "${icon}" "${APP}/Contents/Resources/${ID}.icns")
        set(ICON_KEYS "    <key>CFBundleIconFile</key>\n    <string>${ID}</string>\n")
    endif()
endif()
if(NOT ICON_KEYS AND GAME_EXE AND PE_ICON AND EXISTS "${GAME_EXE}")
    set(ico "${APP}/Contents/Resources/${ID}.ico")
    execute_process(COMMAND "${PE_ICON}" "${GAME_EXE}" "${ico}" RESULT_VARIABLE got)
    if(got EQUAL 0)
        execute_process(COMMAND sips -s format icns "${ico}" --out "${APP}/Contents/Resources/${ID}.icns"
                        RESULT_VARIABLE converted OUTPUT_QUIET)
        if(converted EQUAL 0)
            set(ICON_KEYS "    <key>CFBundleIconFile</key>\n    <string>${ID}</string>\n")
        endif()
    endif()
    file(REMOVE "${ico}")
endif()

# The title in XML.
string(REPLACE "&" "&amp;" title "${TITLE}")
string(REPLACE "'" "&apos;" title "${title}")

file(WRITE "${APP}/Contents/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundleExecutable</key>
    <string>${ID}</string>
    <key>CFBundleIdentifier</key>
    <string>local.scimitar.${ID}</string>
    <key>CFBundleName</key>
    <string>${NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${title}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
${ICON_KEYS}    <key>LSApplicationCategoryType</key>
    <string>public.app-category.action-games</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>macOS asks this of every app that plays sound through Core Audio. The game does not record anything.</string>
</dict>
</plist>
")

execute_process(COMMAND codesign --force --sign - "${APP}" RESULT_VARIABLE signed)
if(NOT signed EQUAL 0)
    message(WARNING "codesign failed for ${APP}")
endif()
