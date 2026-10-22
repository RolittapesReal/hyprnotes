find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Sql Svg Test)
# Standalone builds (-DHN_MODULES=app) still need the real libraries.
foreach(dep core theme editor platform mods plugins)
  if(NOT TARGET hn_${dep} AND EXISTS ${CMAKE_SOURCE_DIR}/src/${dep}/CMakeLists.txt)
    add_subdirectory(${CMAKE_SOURCE_DIR}/src/${dep} ${CMAKE_BINARY_DIR}/_dep_${dep})
  endif()
endforeach()

add_library(hn_app STATIC
  cli.cpp signal_bridge.cpp ui_common.cpp shortcut_sheet.cpp action_row.cpp note_state.cpp note_session.cpp editor_bridge.cpp tag_edit.cpp controller.cpp
  note_list.cpp status_strip.cpp organizer_window.cpp sticky_window.cpp settings_dialog.cpp
  native_adapter.cpp plugin_bridges.cpp plugin_service.cpp consent_dialog.cpp command_palette.cpp plugins_page.cpp market_model.cpp market_page.cpp panel_dock.cpp editor_hooks.cpp link_rename.cpp
  cli.h signal_bridge.h policy.h ui_common.h action_row.h note_state.h note_session.h editor_bridge.h tag_edit.h controller.h
  note_list.h status_strip.h organizer_window.h sticky_window.h settings_dialog.h
  native_adapter.h plugin_bridges.h plugin_service.h consent_dialog.h command_palette.h plugins_page.h market_model.h market_page.h panel_dock.h editor_hooks.h link_rename.h)
target_include_directories(hn_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(hn_app PUBLIC HN_VERSION="${PROJECT_VERSION}")
target_link_libraries(hn_app PUBLIC Qt6::Widgets Qt6::Network hn_editor hn_theme hn_core hn_platform hn_mods hn_plugins)

add_executable(hyprnotes main.cpp)
target_link_libraries(hyprnotes PRIVATE hn_app)
install(TARGETS hyprnotes RUNTIME DESTINATION bin)

# Tests: tests/unit/app_*_test.cpp, one executable each (offscreen by default).
# The two popup/focus suites can select a supported display for platform-specific
# coverage without changing the rest of the app test harness.
set(HN_POLISH_TEST_QPA_PLATFORM "offscreen" CACHE STRING "Qt platform for app polish tests")
file(GLOB HN_APP_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/app_*_test.cpp)
foreach(src ${HN_APP_TESTS})
  get_filename_component(t ${src} NAME_WE)
  add_executable(${t} ${src})
  target_link_libraries(${t} PRIVATE hn_app Qt6::Test)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit)
  target_compile_definitions(${t} PRIVATE HN_BIN="$<TARGET_FILE:hyprnotes>" HN_SOURCE_DIR="${CMAKE_SOURCE_DIR}" HN_PLUGINS_DIR="${HN_PLUGINS_DIR}" HN_PLUGIN_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/fixtures/plugins"
    HN_EXAMPLE_MOD_DIR="${CMAKE_BINARY_DIR}/example-uppercase-selection" HN_EXAMPLE_MOD_SRC="${CMAKE_SOURCE_DIR}/examples/mods/uppercase-selection")
  add_dependencies(${t} hyprnotes)
  if(TARGET hn_example_uppercase_selection)
    add_dependencies(${t} hn_example_uppercase_selection)
  endif()
  set(test_qpa "offscreen")
  if(t STREQUAL "app_editor_polish_test" OR t STREQUAL "app_theme_polish_test")
    set(test_qpa "${HN_POLISH_TEST_QPA_PLATFORM}")
  endif()
  if(t STREQUAL "app_a11y_test")
    # Run each QtTest slot separately so no combined test invocation exceeds the fixed timeout.
    set(a11y_cases
      settingsControlsHaveNamesAndCompleteTabCycle
      settingsPagesStayWithinNarrowLayoutAtEverySupportedFontSize
      pluginPagesExposeNamesAndCompleteTabCycle
      consentControlsHaveNamesSafeEscapeAndDefault
      nativeConsentControlsHaveNamesAndSafeEscape
      consentUsesNinetyPercentOf1366By768OffscreenScreen
      commandPaletteSearchHasNameAndEscapeDismisses
      firstRunFolderDialogHasNamedControlsAndEscapeCancels
      themeImportPickerExposesKeyboardControlsAndGeometry
      themeExportPickerExposesKeyboardControlsAndGeometry
      notesFolderPickerExposesKeyboardControlsAndGeometry
      pluginFilePickerExposesKeyboardControlsAndGeometry
      pluginFolderPickerExposesKeyboardControlsAndGeometry
      firstRunFolderPickerExposesKeyboardControlsAndGeometry
      linkRenameConfirmationDefaultsToCancelAndEscapeIsSafe
      pluginBridgePromptEscapeCancelsAndNamesInput
      pluginBridgeConfirmEscapeUsesSafeDefault
      renameInputEscapeLeavesNoteUnchanged
      tagInputEscapeLeavesNoteUnchanged
      deleteConfirmationHasSafeDefaultAndEscapeKeepsFile
      pluginRemovalConfirmationHasSafeDefaultAndEscapeKeepsPlugin
      themeRemovalConfirmationDefaultsToNoAndEscapeKeepsTheme
      recoveryConfirmationDefaultsToLaterAndEscapePreservesDraft
      duplicatePluginReplacementDefaultsToNoAndEscapeKeepsInstalledPlugin
      pluginAuditDialogNamesControlsAndEscapeCloses
      pluginBridgePickEscapeCancelsAndNamesList
      builtInThemeTextAndIndicatorPairsMeetContrastThresholds)
    foreach(test_case IN LISTS a11y_cases)
      add_test(NAME "${t}_${test_case}" COMMAND "$<TARGET_FILE:${t}>" "${test_case}")
      set_tests_properties("${t}_${test_case}" PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${test_qpa}" TIMEOUT 120)
    endforeach()
  else()
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${test_qpa}" TIMEOUT 120)
  endif()
  if(t STREQUAL "app_a11y_test")
    # QWindowSystemInterface is test-only here, to resize offscreen QScreen without touching the host desktop.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6GuiPrivate REQUIRED CONFIG)
    target_link_libraries(${t} PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(${t} PRIVATE HN_QPA_SCREEN_TEST_API)
  endif()
endforeach()
