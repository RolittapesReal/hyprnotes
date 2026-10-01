find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)

add_library(hn_plugins STATIC
  manifest.cpp registry.cpp trust.cpp package.cpp luahost.cpp luapattern.cpp luapattern.h manager.cpp
  include/hn/plugins/types.h include/hn/plugins/store.h include/hn/plugins/runtime.h)
target_include_directories(hn_plugins PUBLIC include)
target_link_libraries(hn_plugins PUBLIC Qt6::Core PRIVATE PkgConfig::LUA PkgConfig::LIBARCHIVE PkgConfig::LUA)

foreach(t manifest package trust runtime sandbox hardening examples docs)
  add_executable(plugins_${t}_test ${CMAKE_SOURCE_DIR}/tests/unit/plugins_${t}_test.cpp)
  target_link_libraries(plugins_${t}_test PRIVATE hn_plugins Qt6::Test PkgConfig::LIBARCHIVE PkgConfig::LUA)
  target_compile_definitions(plugins_${t}_test PRIVATE HN_PLUGIN_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/fixtures/plugins"
    HN_PLUGIN_EXAMPLES="${CMAKE_SOURCE_DIR}/examples/plugins" HN_PLUGIN_DOCS="${CMAKE_SOURCE_DIR}/docs")
  add_test(NAME plugins_${t} COMMAND plugins_${t}_test)
  set_tests_properties(plugins_${t} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen TIMEOUT 120)
endforeach()
