set(HN_FUZZ_NAMES
  markdown_roundtrip frontmatter links query_spec fts_query manifest plugin_archive
  theme_import config ipc_transport hypr_events session market_index)

function(hn_fuzz name)
  add_executable(fuzz_${name} fuzz_${name}.cpp support.cpp)
  target_include_directories(fuzz_${name} PRIVATE ${CMAKE_SOURCE_DIR}/src/editor ${CMAKE_SOURCE_DIR}/src/theme)
  target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer-no-link,address,undefined -g -O1)
  target_link_options(fuzz_${name} PRIVATE -fsanitize=address,undefined)
  target_link_libraries(fuzz_${name} PRIVATE ${ARGN} "${HN_FUZZ_RUNTIME_ARCHIVE}")
endfunction()

if(HN_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "HN_FUZZ requires Clang to provide the libFuzzer runtime")
  endif()
  if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "HN_FUZZ currently supports native builds only")
  endif()
  if(DEFINED CMAKE_CXX_COMPILER_TARGET AND NOT "${CMAKE_CXX_COMPILER_TARGET}" STREQUAL "")
    message(FATAL_ERROR
      "HN_FUZZ currently supports native builds only; CMAKE_CXX_COMPILER_TARGET is set to '${CMAKE_CXX_COMPILER_TARGET}'")
  endif()

  set(HN_FUZZ_TOOLCHAIN_FLAGS
    "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_COMPILER_ARG1} ${CMAKE_EXE_LINKER_FLAGS}")
  if(CMAKE_CONFIGURATION_TYPES)
    set(HN_FUZZ_CONFIGS ${CMAKE_CONFIGURATION_TYPES})
  elseif(CMAKE_BUILD_TYPE)
    set(HN_FUZZ_CONFIGS ${CMAKE_BUILD_TYPE})
  else()
    set(HN_FUZZ_CONFIGS)
  endif()
  foreach(HN_FUZZ_CONFIG IN LISTS HN_FUZZ_CONFIGS)
    string(TOUPPER "${HN_FUZZ_CONFIG}" HN_FUZZ_CONFIG_UPPER)
    string(APPEND HN_FUZZ_TOOLCHAIN_FLAGS
      " ${CMAKE_CXX_FLAGS_${HN_FUZZ_CONFIG_UPPER}} ${CMAKE_EXE_LINKER_FLAGS_${HN_FUZZ_CONFIG_UPPER}}")
  endforeach()
  if(HN_FUZZ_TOOLCHAIN_FLAGS MATCHES
     "(^|[ \t,;])(--target|-target)(=|[ \t,;]|$)|(^|[ \t,;])(-m32|-m64|-mx32)([ \t,;]|$)|(^|[ \t,;])-mabi(=|[ \t,;]|$)")
    message(FATAL_ERROR
      "HN_FUZZ currently supports native builds only; remove target or ABI overrides from compiler and executable linker flags")
  endif()

  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
    RESULT_VARIABLE HN_FUZZ_TARGET_RESULT
    OUTPUT_VARIABLE HN_FUZZ_TARGET_TRIPLE
    ERROR_VARIABLE HN_FUZZ_TARGET_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT "${HN_FUZZ_TARGET_RESULT}" STREQUAL "0" OR HN_FUZZ_TARGET_TRIPLE STREQUAL "")
    message(FATAL_ERROR
      "HN_FUZZ could not determine the Clang target triple: ${HN_FUZZ_TARGET_ERROR}")
  endif()

  string(TOLOWER "${CMAKE_SYSTEM_NAME}" HN_FUZZ_SYSTEM_NAME)
  if(NOT HN_FUZZ_SYSTEM_NAME STREQUAL "linux")
    message(FATAL_ERROR
      "HN_FUZZ currently supports Linux only; configured system is '${CMAKE_SYSTEM_NAME}'")
  endif()

  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" HN_FUZZ_SYSTEM_PROCESSOR)
  if(HN_FUZZ_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    set(HN_FUZZ_SYSTEM_ARCH x86_64)
  elseif(HN_FUZZ_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(HN_FUZZ_SYSTEM_ARCH aarch64)
  elseif(HN_FUZZ_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    set(HN_FUZZ_SYSTEM_ARCH i386)
  else()
    message(FATAL_ERROR
      "HN_FUZZ does not support configured system processor '${CMAKE_SYSTEM_PROCESSOR}'")
  endif()

  if(HN_FUZZ_TARGET_TRIPLE MATCHES "^(x86_64|amd64)(-|$)")
    set(HN_FUZZ_RUNTIME_ARCH x86_64)
    set(HN_FUZZ_TARGET_ARCH x86_64)
  elseif(HN_FUZZ_TARGET_TRIPLE MATCHES "^(aarch64|arm64)(-|$)")
    set(HN_FUZZ_RUNTIME_ARCH aarch64)
    set(HN_FUZZ_TARGET_ARCH aarch64)
  elseif(HN_FUZZ_TARGET_TRIPLE MATCHES "^(i[3-6]86|x86)(-|$)")
    set(HN_FUZZ_RUNTIME_ARCH i386)
    set(HN_FUZZ_TARGET_ARCH i386)
  else()
    message(FATAL_ERROR
      "HN_FUZZ does not know the Clang Fuzzer runtime archive for target ${HN_FUZZ_TARGET_TRIPLE}")
  endif()
  if(NOT HN_FUZZ_TARGET_ARCH STREQUAL HN_FUZZ_SYSTEM_ARCH)
    message(FATAL_ERROR
      "HN_FUZZ Clang target architecture '${HN_FUZZ_TARGET_ARCH}' does not match configured processor '${CMAKE_SYSTEM_PROCESSOR}'")
  endif()
  if(NOT HN_FUZZ_TARGET_TRIPLE MATCHES "-linux(-|$)")
    message(FATAL_ERROR
      "HN_FUZZ Clang target '${HN_FUZZ_TARGET_TRIPLE}' does not match the configured Linux system")
  endif()

  set(HN_FUZZ_RUNTIME_NAME "libclang_rt.fuzzer-${HN_FUZZ_RUNTIME_ARCH}.a")
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${HN_FUZZ_RUNTIME_NAME}"
    RESULT_VARIABLE HN_FUZZ_RUNTIME_RESULT
    OUTPUT_VARIABLE HN_FUZZ_RUNTIME_ARCHIVE
    ERROR_VARIABLE HN_FUZZ_RUNTIME_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT "${HN_FUZZ_RUNTIME_RESULT}" STREQUAL "0"
     OR NOT IS_ABSOLUTE "${HN_FUZZ_RUNTIME_ARCHIVE}"
     OR NOT EXISTS "${HN_FUZZ_RUNTIME_ARCHIVE}"
     OR IS_DIRECTORY "${HN_FUZZ_RUNTIME_ARCHIVE}")
    message(FATAL_ERROR
      "HN_FUZZ could not resolve Clang runtime ${HN_FUZZ_RUNTIME_NAME}; "
      "compiler returned '${HN_FUZZ_RUNTIME_ARCHIVE}': ${HN_FUZZ_RUNTIME_ERROR}")
  endif()

  hn_fuzz(markdown_roundtrip hn_editor)
  target_compile_definitions(fuzz_markdown_roundtrip PRIVATE HN_FUZZ_GUI=1)
  add_executable(fuzz_markdown_lifecycle_diagnostic
    lifecycle_diagnostic.cpp fuzz_markdown_roundtrip.cpp support.cpp)
  target_include_directories(fuzz_markdown_lifecycle_diagnostic PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor ${CMAKE_SOURCE_DIR}/src/theme)
  target_compile_definitions(fuzz_markdown_lifecycle_diagnostic PRIVATE
    HN_FUZZ_GUI=1 HN_FUZZ_REGRESSION_BUILD=1
    HN_FUZZ_LIFECYCLE_DIAGNOSTIC=1)
  target_compile_options(fuzz_markdown_lifecycle_diagnostic PRIVATE
    -fsanitize=address,undefined -g -O1)
  target_link_options(fuzz_markdown_lifecycle_diagnostic PRIVATE
    -fsanitize=address,undefined)
  target_link_libraries(fuzz_markdown_lifecycle_diagnostic PRIVATE hn_editor)
  set(HN_FUZZ_LIFECYCLE_SEED "${CMAKE_CURRENT_BINARY_DIR}/fuzz-markdown-lifecycle-seed.md")
  file(WRITE "${HN_FUZZ_LIFECYCLE_SEED}" "markdown callback lifecycle\n")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuzz-lifecycle-tmp")
  add_test(NAME fuzz_markdown_lifecycle
    COMMAND fuzz_markdown_lifecycle_diagnostic "${HN_FUZZ_LIFECYCLE_SEED}")
  set_tests_properties(fuzz_markdown_lifecycle PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/fuzz-lifecycle-tmp"
    TIMEOUT 30)
  hn_fuzz(frontmatter hn_core)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuzz-shared-harness-tmp")
  add_test(NAME fuzz_shared_support_process_exit
    COMMAND fuzz_frontmatter -runs=2 "${HN_FUZZ_LIFECYCLE_SEED}")
  set_tests_properties(fuzz_shared_support_process_exit PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/fuzz-shared-harness-tmp"
    TIMEOUT 30)
  hn_fuzz(links hn_core)
  hn_fuzz(query_spec hn_core)
  hn_fuzz(fts_query hn_core)
  add_executable(fuzz_fts_lifecycle_diagnostic
    fts_lifecycle_diagnostic.cpp fuzz_fts_query.cpp support.cpp)
  target_compile_definitions(fuzz_fts_lifecycle_diagnostic PRIVATE
    HN_FUZZ_REGRESSION_BUILD=1 HN_FUZZ_LIFECYCLE_DIAGNOSTIC=1)
  target_compile_options(fuzz_fts_lifecycle_diagnostic PRIVATE
    -fsanitize=address,undefined -g -O1)
  target_link_options(fuzz_fts_lifecycle_diagnostic PRIVATE
    -fsanitize=address,undefined)
  target_link_libraries(fuzz_fts_lifecycle_diagnostic PRIVATE hn_core)
  add_test(NAME fuzz_fts_lifecycle COMMAND fuzz_fts_lifecycle_diagnostic)
  set_tests_properties(fuzz_fts_lifecycle PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
  hn_fuzz(manifest hn_plugins)
  hn_fuzz(plugin_archive hn_plugins)
  hn_fuzz(theme_import hn_theme)
  target_compile_definitions(fuzz_theme_import PRIVATE HN_FUZZ_GUI=1)
  add_executable(fuzz_theme_import_lifecycle_diagnostic
    theme_lifecycle_diagnostic.cpp fuzz_theme_import.cpp support.cpp)
  target_include_directories(fuzz_theme_import_lifecycle_diagnostic PRIVATE
    ${CMAKE_SOURCE_DIR}/src/theme)
  target_compile_definitions(fuzz_theme_import_lifecycle_diagnostic PRIVATE
    HN_FUZZ_GUI=1 HN_FUZZ_REGRESSION_BUILD=1
    HN_FUZZ_LIFECYCLE_DIAGNOSTIC=1)
  target_compile_options(fuzz_theme_import_lifecycle_diagnostic PRIVATE
    -fsanitize=address,undefined -g -O1)
  target_link_options(fuzz_theme_import_lifecycle_diagnostic PRIVATE
    -fsanitize=address,undefined)
  target_link_libraries(fuzz_theme_import_lifecycle_diagnostic PRIVATE hn_theme)
  add_test(NAME fuzz_theme_import_lifecycle
    COMMAND fuzz_theme_import_lifecycle_diagnostic
      "${CMAKE_CURRENT_SOURCE_DIR}/corpus/theme_import/base16.yaml")
  set_tests_properties(fuzz_theme_import_lifecycle PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
  hn_fuzz(config hn_theme)
  hn_fuzz(ipc_transport hn_platform)
  hn_fuzz(hypr_events hn_platform)
  hn_fuzz(session hn_platform)
  hn_fuzz(market_index hn_plugins)
endif()

# Always built: replay archived crash inputs in regular GCC/Clang builds too.
if(TARGET hn_core AND TARGET hn_editor AND TARGET hn_plugins AND TARGET hn_platform AND TARGET hn_theme AND TARGET hn_mods)
add_executable(fuzz_regressions regression_main.cpp
  fuzz_markdown_roundtrip.cpp fuzz_frontmatter.cpp fuzz_links.cpp fuzz_query_spec.cpp
  fuzz_fts_query.cpp fuzz_manifest.cpp fuzz_plugin_archive.cpp fuzz_theme_import.cpp
  fuzz_config.cpp fuzz_ipc_transport.cpp fuzz_hypr_events.cpp fuzz_session.cpp fuzz_market_index.cpp
  support.cpp)
target_compile_definitions(fuzz_regressions PRIVATE HN_FUZZ_REGRESSION_BUILD=1
  HN_FUZZ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(fuzz_regressions PRIVATE ${CMAKE_SOURCE_DIR}/src/editor ${CMAKE_SOURCE_DIR}/src/theme)
target_link_libraries(fuzz_regressions PRIVATE hn_editor hn_core hn_plugins hn_theme hn_platform hn_mods)
add_test(NAME fuzz_regressions COMMAND fuzz_regressions)
set_tests_properties(fuzz_regressions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endif()
