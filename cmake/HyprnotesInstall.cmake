# Install rules for everything except the executable (installed in src/app/CMakeLists.txt).
include(GNUInstallDirs)
set(_p ${PROJECT_SOURCE_DIR}/packaging)

install(FILES ${_p}/hyprnotes.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
foreach(s 16 32 48 128 256)
  install(FILES ${_p}/icons/hyprnotes-${s}.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${s}x${s}/apps RENAME hyprnotes.png)
endforeach()
install(FILES ${_p}/icons/hyprnotes.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

set(_share ${CMAKE_INSTALL_DATADIR}/hyprnotes)
install(FILES ${_p}/share/themes/modernist-example.json DESTINATION ${_share}/themes)
install(FILES ${_p}/share/autostart/hyprnotes.desktop DESTINATION ${_share}/autostart)
install(DIRECTORY ${_p}/share/hyprland/ DESTINATION ${_share}/hyprland)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/examples/mods/uppercase-selection DESTINATION ${_share}/examples/mods)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/examples/plugins/ DESTINATION ${_share}/examples/plugins)
install(FILES ${PROJECT_SOURCE_DIR}/src/mods/include/hyprnotes/mod_api.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hyprnotes)

install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/docs/install.md
              ${PROJECT_SOURCE_DIR}/docs/mods.md ${PROJECT_SOURCE_DIR}/docs/plugins.md ${PROJECT_SOURCE_DIR}/docs/plugin-api.md ${PROJECT_SOURCE_DIR}/docs/theme-reference.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/hyprnotes)
