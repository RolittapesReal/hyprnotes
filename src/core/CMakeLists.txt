find_package(PkgConfig REQUIRED)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
# 3.35 is the minimum: search uses WITH ... AS MATERIALIZED (also enforced at runtime, see LibraryIndex::sqliteVersionSupported)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3>=3.35)

add_library(hn_core STATIC
  markdown_codec.cpp paths.cpp fs_util.cpp worker.cpp
  recovery_store.cpp note_repository.cpp library_index.cpp
  frontmatter.cpp links.cpp query_engine.cpp
  include/hn/core/note_repository.h include/hn/core/library_index.h
  include/hn/core/links.h include/hn/core/frontmatter.h include/hn/core/query_engine.h)
target_include_directories(hn_core PUBLIC include)
target_link_libraries(hn_core PUBLIC Qt6::Core Qt6::Sql Qt6::Network PRIVATE PkgConfig::MD4C PkgConfig::SQLITE3)

foreach(t markdown_codec paths repository recovery index links frontmatter graph query)
  add_executable(core_${t}_test ${CMAKE_SOURCE_DIR}/tests/unit/core_${t}_test.cpp)
  target_link_libraries(core_${t}_test PRIVATE hn_core Qt6::Test)
  add_test(NAME core_${t} COMMAND core_${t}_test)
endforeach()

# The unchanged 100k-save regression can approach QtTest's default five-minute
# function limit in instrumented builds. Extend only those bounded test runs;
# ordinary Release deadlines remain unchanged.
if(HN_COVERAGE OR HN_SANITIZE)
  set_tests_properties(core_recovery PROPERTIES
    ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=900000"
    TIMEOUT 900)
endif()
