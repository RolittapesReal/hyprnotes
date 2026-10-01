find_package(PkgConfig REQUIRED)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)

add_library(hn_core STATIC
  markdown_codec.cpp paths.cpp fs_util.cpp worker.cpp
  recovery_store.cpp note_repository.cpp library_index.cpp
  include/hn/core/note_repository.h include/hn/core/library_index.h)
target_include_directories(hn_core PUBLIC include)
target_link_libraries(hn_core PUBLIC Qt6::Core Qt6::Sql Qt6::Network PRIVATE PkgConfig::MD4C)

foreach(t markdown_codec paths repository recovery index)
  add_executable(core_${t}_test ${CMAKE_SOURCE_DIR}/tests/unit/core_${t}_test.cpp)
  target_link_libraries(core_${t}_test PRIVATE hn_core Qt6::Test)
  add_test(NAME core_${t} COMMAND core_${t}_test)
endforeach()
