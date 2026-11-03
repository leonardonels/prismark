# prismark-gui: the desktop app (Qt Widgets; Qt 6 preferred, Qt 5.15 accepted).
# It launches the `prismark` runner built next to it, so both are built together.

set(PRISMARK_GUI AUTO CACHE STRING "Build the desktop app: AUTO, ON or OFF")
set_property(CACHE PRISMARK_GUI PROPERTY STRINGS AUTO ON OFF)

if(PRISMARK_DESKTOP AND NOT PRISMARK_GUI STREQUAL "OFF")
  # C++ must be enabled before Qt is looked up: Qt's package config runs C++ compile checks (FindWrapAtomic).
  include(CheckLanguage)
  check_language(CXX)
  if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets Svg QUIET)
  endif()
  if(QT_FOUND)
    find_package(Qt${QT_VERSION_MAJOR} 5.15 COMPONENTS Widgets Svg REQUIRED)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS DBus QUIET)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    set(_font_dir ${CMAKE_BINARY_DIR}/_deps/fonts)
    set(_font_files "")
    # Archivo for text, Saira for scores and headings (both variable: width and weight), IBM Plex Mono for units and
    # IDs (all SIL OFL 1.1), from the Google Fonts repository at a pinned commit, compiled into the app.
    set(_gf https://raw.githubusercontent.com/google/fonts/6157dfaa640ca5738b4d1a7070fe389c3036d7fc/ofl)
    foreach(f IN ITEMS "archivo/Archivo%5Bwdth,wght%5D.ttf|Archivo.ttf|0e094a7d3c7c4c25cf1310c4b30014f1dae9332220b1c2c88f4fa996f0b05053"
                       "saira/Saira%5Bwdth,wght%5D.ttf|Saira.ttf|9d050fc5a01c85f74c4257c207d50b55d1e40c37308c642f974a2c5003231dde"
                       "ibmplexmono/IBMPlexMono-Regular.ttf|IBMPlexMono-Regular.ttf|6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46"
                       "ibmplexmono/IBMPlexMono-Medium.ttf|IBMPlexMono-Medium.ttf|a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755"
                       "ibmplexmono/IBMPlexMono-SemiBold.ttf|IBMPlexMono-SemiBold.ttf|d3c38e55c78f5b0f28009fddba4834ec503278936a5986032424c9bd2d23aa46")
      string(REPLACE "|" ";" parts "${f}")
      list(GET parts 0 src)
      list(GET parts 1 name)
      list(GET parts 2 h)
      file(DOWNLOAD ${_gf}/${src} ${_font_dir}/${name} EXPECTED_HASH SHA256=${h} STATUS st)
      list(GET st 0 code)
      if(code EQUAL 0)
        string(APPEND _font_files "    <file alias=\"fonts/${name}\">${_font_dir}/${name}</file>\n")
      else()
        message(WARNING "${name} not fetched (${st}); the app falls back to the system font")
      endif()
    endforeach()
    # Reference systems: every file in references/ (references/README.md). The glob re-runs at build time,
    # so adding or removing a file needs no reconfigure.
    file(GLOB _reference_files CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/references/*.json)
    set(_reference_entries "")
    foreach(f IN LISTS _reference_files)
      get_filename_component(n ${f} NAME)
      string(APPEND _reference_entries "    <file alias=\"references/${n}\">${f}</file>\n")
    endforeach()
    # The logo, drawn from vectors at every size it is shown (apps/gui/icons).
    set(_logo_entries "    <file alias=\"prismark.svg\">${PROJECT_SOURCE_DIR}/apps/gui/icons/prismark.svg</file>\n")
    string(APPEND _logo_entries "    <file alias=\"prismark-app.svg\">${PROJECT_SOURCE_DIR}/apps/gui/icons/prismark-app.svg</file>\n")
    string(APPEND _logo_entries "    <file alias=\"prismark-app-small.svg\">${PROJECT_SOURCE_DIR}/apps/gui/icons/prismark-app-small.svg</file>\n")
    file(WRITE ${CMAKE_BINARY_DIR}/generated/prismark-gui.qrc
      "<RCC>\n  <qresource prefix=\"/\">\n${_font_files}${_reference_entries}${_logo_entries}  </qresource>\n</RCC>\n")

    # On Windows the icon of the program file, which Explorer and the taskbar show (drawn by the app from the SVG:
    # PRISMARK_GUI_ICO, apps/gui/main.cpp).
    set(_win_icon "")
    if(WIN32)
      file(WRITE ${CMAKE_BINARY_DIR}/generated/prismark-gui.rc
        "IDI_ICON1 ICON \"${PROJECT_SOURCE_DIR}/apps/gui/icons/prismark.ico\"\n")
      set(_win_icon ${CMAKE_BINARY_DIR}/generated/prismark-gui.rc)
    endif()

    add_executable(prismark-gui WIN32 MACOSX_BUNDLE
      apps/gui/main.cpp apps/gui/mainwindow.cpp apps/gui/metrics.cpp apps/gui/references.cpp apps/gui/widgets.cpp
      apps/gui/theme.cpp apps/gui/setup.cpp apps/gui/inputwatch.cpp apps/gui/stub.cpp ${CMAKE_BINARY_DIR}/generated/prismark-gui.qrc
      ${_win_icon})
    set_target_properties(prismark-gui PROPERTIES AUTOMOC ON AUTORCC ON
      MACOSX_BUNDLE_BUNDLE_NAME Prismark MACOSX_BUNDLE_GUI_IDENTIFIER org.prismark.app
      MACOSX_BUNDLE_ICON_FILE AppIcon MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
      MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    target_compile_definitions(prismark-gui PRIVATE PMK_VERSION="${PROJECT_VERSION}"
      PMK_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(prismark-gui PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Svg)
    if(WIN32)
      target_link_libraries(prismark-gui PRIVATE shell32) # ShellExecuteEx: restart as administrator
    endif()
    if(TARGET Qt${QT_VERSION_MAJOR}::DBus AND UNIX AND NOT APPLE)
      target_link_libraries(prismark-gui PRIVATE Qt${QT_VERSION_MAJOR}::DBus)
      target_compile_definitions(prismark-gui PRIVATE PMK_HAVE_DBUS)
    endif()
    install(PROGRAMS tools/k1x/prepare.py DESTINATION share/prismark)
    add_dependencies(prismark-gui prismark_cli)
    if(APPLE)
      # The app looks for the runner in its own directory. Its icon is the layered glass one of macOS 26 and later, the
      # same design as the iOS app's, with a flat render for older macOS. Compiled ahead of time, as the CI machine's
      # Xcode cannot read the format. After changing the icon:
      #   xcrun actool apps/ios/Prismark/Prismark/AppIcon.icon --compile apps/gui/icons/mac --platform macosx \
      #     --minimum-deployment-target 13.0 --app-icon AppIcon --output-partial-info-plist /tmp/icon.plist
      add_custom_command(TARGET prismark-gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:prismark_cli> $<TARGET_FILE_DIR:prismark-gui>/
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:prismark-gui>/Resources
        COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/apps/gui/icons/mac/Assets.car
                ${PROJECT_SOURCE_DIR}/apps/gui/icons/mac/AppIcon.icns $<TARGET_BUNDLE_CONTENT_DIR:prismark-gui>/Resources/
        COMMAND sh -c "/usr/libexec/PlistBuddy -c 'Delete :CFBundleIconName' \"$1\" 2>/dev/null || true && /usr/libexec/PlistBuddy -c 'Add :CFBundleIconName string AppIcon' \"$1\"" -- $<TARGET_BUNDLE_CONTENT_DIR:prismark-gui>/Info.plist
        # The Finder keeps a bundle's icon until the bundle itself changes: touch it so the new one is shown.
        COMMAND ${CMAKE_COMMAND} -E touch $<TARGET_BUNDLE_DIR:prismark-gui>
        COMMAND_EXPAND_LISTS VERBATIM)
    endif()
    install(TARGETS prismark-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
    if(UNIX AND NOT APPLE)
      install(FILES apps/gui/prismark.desktop DESTINATION share/applications)
      install(FILES apps/gui/icons/prismark-app.svg DESTINATION share/icons/hicolor/scalable/apps RENAME prismark.svg)
    endif()
    message(STATUS "GUI: prismark-gui with Qt ${Qt${QT_VERSION_MAJOR}_VERSION}")
  elseif(PRISMARK_GUI STREQUAL "ON")
    message(FATAL_ERROR "PRISMARK_GUI=ON but Qt (Widgets and Svg) was not found")
  else()
    message(STATUS "GUI: Qt Widgets and Svg not found; the desktop app is not built "
      "(install qt6-base-dev and qt6-svg-dev, or qtbase5-dev and libqt5svg5-dev)")
  endif()
endif()
