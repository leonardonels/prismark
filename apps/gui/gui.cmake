# prismark-gui: the desktop app (Qt Widgets; Qt 6 preferred, Qt 5.15 accepted).
# It launches the `prismark` runner built next to it, so both are built together.

set(PRISMARK_GUI AUTO CACHE STRING "Build the desktop app: AUTO, ON or OFF")
set_property(CACHE PRISMARK_GUI PROPERTY STRINGS AUTO ON OFF)

if(PRISMARK_DESKTOP AND NOT PRISMARK_GUI STREQUAL "OFF")
  find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets QUIET)
  if(QT_FOUND)
    find_package(Qt${QT_VERSION_MAJOR} 5.15 COMPONENTS Widgets REQUIRED)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS DBus QUIET)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # Inter typeface (SIL OFL 1.1), pinned and verified like the kernel sources, compiled into the app.
    set(_inter_dir ${CMAKE_BINARY_DIR}/_deps/inter)
    set(_inter_files "")
    foreach(f IN ITEMS Regular:a7e791e8f5a0fb02b65663f7fca73e1d1ca9543f772ad480cbd76f4e3fe3f8cc
                       Medium:99dab2bdcb613c4c8264000a94351d1227f74dc95a86d1249493aeee0c0179c4
                       SemiBold:8c1990b6012254ea2b487161697d107357dd0ee55811cfd91c8c11227bbef457
                       Bold:1e9dfd6a6e33ac63a8fe3b4ed7ae0df9eac2d0b25e444e8e7daef4ac77943fc1)
      string(REPLACE ":" ";" pair ${f})
      list(GET pair 0 w)
      list(GET pair 1 h)
      file(DOWNLOAD https://raw.githubusercontent.com/rsms/inter/v3.19/docs/font-files/Inter-${w}.otf
        ${_inter_dir}/Inter-${w}.otf EXPECTED_HASH SHA256=${h} STATUS st)
      list(GET st 0 code)
      if(code EQUAL 0)
        string(APPEND _inter_files "    <file alias=\"fonts/Inter-${w}.otf\">${_inter_dir}/Inter-${w}.otf</file>\n")
      else()
        message(WARNING "Inter ${w} not fetched (${st}); the app falls back to the system font")
      endif()
    endforeach()
    file(WRITE ${CMAKE_BINARY_DIR}/generated/prismark-gui.qrc
      "<RCC>\n  <qresource prefix=\"/\">\n${_inter_files}  </qresource>\n</RCC>\n")

    add_executable(prismark-gui WIN32 MACOSX_BUNDLE
      apps/gui/main.cpp apps/gui/mainwindow.cpp apps/gui/metrics.cpp apps/gui/references.cpp apps/gui/widgets.cpp
      apps/gui/theme.cpp apps/gui/setup.cpp apps/gui/inputwatch.cpp ${CMAKE_BINARY_DIR}/generated/prismark-gui.qrc)
    set_target_properties(prismark-gui PROPERTIES AUTOMOC ON AUTORCC ON
      MACOSX_BUNDLE_BUNDLE_NAME Prismark MACOSX_BUNDLE_GUI_IDENTIFIER org.prismark.app)
    target_compile_definitions(prismark-gui PRIVATE PMK_VERSION="${PROJECT_VERSION}"
      PMK_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(prismark-gui PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
    if(TARGET Qt${QT_VERSION_MAJOR}::DBus AND UNIX AND NOT APPLE)
      target_link_libraries(prismark-gui PRIVATE Qt${QT_VERSION_MAJOR}::DBus)
      target_compile_definitions(prismark-gui PRIVATE PMK_HAVE_DBUS)
    endif()
    install(PROGRAMS tools/k1x/prepare.py DESTINATION share/prismark)
    add_dependencies(prismark-gui prismark_cli ${PRISMARK_MODULE_TARGETS})
    if(APPLE)
      # The app looks for the runner (and the runner for its tier modules) in its own directory.
      add_custom_command(TARGET prismark-gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:prismark_cli> $<TARGET_FILE_DIR:prismark-gui>/
        COMMAND ${CMAKE_COMMAND} -E copy ${PRISMARK_MODULE_FILES} $<TARGET_FILE_DIR:prismark-gui>/
        COMMAND_EXPAND_LISTS)
    endif()
    install(TARGETS prismark-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
    if(UNIX AND NOT APPLE)
      install(FILES apps/gui/prismark.desktop DESTINATION share/applications)
    endif()
    message(STATUS "GUI: prismark-gui with Qt ${Qt${QT_VERSION_MAJOR}_VERSION}")
  elseif(PRISMARK_GUI STREQUAL "ON")
    message(FATAL_ERROR "PRISMARK_GUI=ON but Qt (Widgets) was not found")
  else()
    message(STATUS "GUI: Qt not found; the desktop app is not built (install qt6-base-dev or qtbase5-dev)")
  endif()
endif()
