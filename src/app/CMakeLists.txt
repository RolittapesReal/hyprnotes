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
  native_adapter.cpp plugin_bridges.cpp plugin_service.cpp consent_dialog.cpp command_palette.cpp plugins_page.cpp panel_dock.cpp editor_hooks.cpp link_rename.cpp
  cli.h signal_bridge.h policy.h ui_common.h action_row.h note_state.h note_session.h editor_bridge.h tag_edit.h controller.h
  note_list.h status_strip.h organizer_window.h sticky_window.h settings_dialog.h
  native_adapter.h plugin_bridges.h plugin_service.h consent_dialog.h command_palette.h plugins_page.h panel_dock.h editor_hooks.h link_rename.h)
target_include_directories(hn_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(hn_app PUBLIC HN_VERSION="${PROJECT_VERSION}")
target_link_libraries(hn_app PUBLIC Qt6::Widgets Qt6::Network hn_editor hn_theme hn_core hn_platform hn_mods hn_plugins)

add_executable(hyprnotes main.cpp)
target_link_libraries(hyprnotes PRIVATE hn_app)
install(TARGETS hyprnotes RUNTIME DESTINATION bin)

# Tests: tests/unit/app_*_test.cpp, one executable each (offscreen).
file(GLOB HN_APP_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/app_*_test.cpp)
foreach(src ${HN_APP_TESTS})
  get_filename_component(t ${src} NAME_WE)
  add_executable(${t} ${src})
  target_link_libraries(${t} PRIVATE hn_app Qt6::Test)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit)
  target_compile_definitions(${t} PRIVATE HN_BIN="$<TARGET_FILE:hyprnotes>" HN_SOURCE_DIR="${CMAKE_SOURCE_DIR}" HN_PLUGIN_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/fixtures/plugins"
    HN_EXAMPLE_MOD_DIR="${CMAKE_BINARY_DIR}/example-uppercase-selection" HN_EXAMPLE_MOD_SRC="${CMAKE_SOURCE_DIR}/examples/mods/uppercase-selection")
  add_dependencies(${t} hyprnotes)
  if(TARGET hn_example_uppercase_selection)
    add_dependencies(${t} hn_example_uppercase_selection)
  endif()
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endforeach()
