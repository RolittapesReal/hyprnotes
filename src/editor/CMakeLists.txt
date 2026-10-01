find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Test)
# Standalone builds (-DHN_MODULES=editor) still need the real core/theme libraries.
foreach(dep core theme)
  if(NOT TARGET hn_${dep} AND EXISTS ${CMAKE_SOURCE_DIR}/src/${dep}/CMakeLists.txt)
    add_subdirectory(${CMAKE_SOURCE_DIR}/src/${dep} ${CMAKE_BINARY_DIR}/_dep_${dep})
  endif()
endforeach()

add_library(hn_editor STATIC history.cpp md_io.cpp visual_edit.cpp note_editor.cpp formatting_toolbar.cpp
  include/hn/editor/history.h include/hn/editor/note_editor.h include/hn/editor/formatting_toolbar.h visual_edit.h)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
target_include_directories(hn_editor PUBLIC include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(hn_editor PUBLIC Qt6::Widgets hn_theme hn_core PRIVATE PkgConfig::MD4C)

file(GLOB HN_EDITOR_TESTS ${CMAKE_SOURCE_DIR}/tests/unit/editor_*_test.cpp)
foreach(src ${HN_EDITOR_TESTS})
  get_filename_component(t ${src} NAME_WE)
  add_executable(${t} ${src})
  target_link_libraries(${t} PRIVATE hn_editor Qt6::Test)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
