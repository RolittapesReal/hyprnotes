find_package(Qt6 REQUIRED COMPONENTS Core Test)
add_library(hn_mods STATIC manifest.cpp modhost.cpp include/hn/mods/mods.h)
target_include_directories(hn_mods PUBLIC include)
target_link_libraries(hn_mods PUBLIC Qt6::Core ${CMAKE_DL_LIBS})

# Separately configured+built example mod (its own CMake project).
set(HN_EX_BIN ${CMAKE_BINARY_DIR}/example-uppercase-selection)
add_custom_target(hn_example_uppercase_selection ALL
  COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR}/examples/mods/uppercase-selection -B ${HN_EX_BIN} -DCMAKE_BUILD_TYPE=Release -G Ninja
  COMMAND ${CMAKE_COMMAND} --build ${HN_EX_BIN}
  COMMENT "Building example mod uppercase-selection")

foreach(v GOOD BAD_VERSION SMALL FAIL)
  string(TOLOWER ${v} lv)
  add_library(mods_fixture_${lv} SHARED ${CMAKE_SOURCE_DIR}/tests/unit/mods_fixture.c)
  target_include_directories(mods_fixture_${lv} PRIVATE include)
  if(NOT v STREQUAL GOOD)
    target_compile_definitions(mods_fixture_${lv} PRIVATE FIX_${v})
  endif()
  set_target_properties(mods_fixture_${lv} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fixtures)
endforeach()

add_executable(mods_test ${CMAKE_SOURCE_DIR}/tests/unit/mods_test.cpp)
target_link_libraries(mods_test PRIVATE hn_mods Qt6::Test)
add_dependencies(mods_test hn_example_uppercase_selection mods_fixture_good mods_fixture_bad_version mods_fixture_small mods_fixture_fail)
target_compile_definitions(mods_test PRIVATE
  HN_EXAMPLE_DIR="${CMAKE_SOURCE_DIR}/examples/mods/uppercase-selection" HN_EXAMPLE_BIN="${HN_EX_BIN}" HN_FIX_DIR="${CMAKE_BINARY_DIR}/fixtures")
add_test(NAME mods_test COMMAND mods_test)
set_tests_properties(mods_test PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
