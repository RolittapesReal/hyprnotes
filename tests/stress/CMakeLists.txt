# P8 stress/load executables. Configure with -DHN_STRESS=ON (Release). Run via tests/stress/run_all.sh.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Test)
foreach(t editor repo index app robust)
  add_executable(stress_${t} stress_${t}.cpp)
  target_link_libraries(stress_${t} PRIVATE hn_app Qt6::Test)
  target_include_directories(stress_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(stress_${t} PRIVATE HN_BIN="$<TARGET_FILE:hyprnotes>"
    HN_EXAMPLE_MOD_DIR="${CMAKE_BINARY_DIR}/example-uppercase-selection" HN_EXAMPLE_MOD_SRC="${CMAKE_SOURCE_DIR}/examples/mods/uppercase-selection")
  add_dependencies(stress_${t} hyprnotes)
  if(TARGET hn_example_uppercase_selection)
    add_dependencies(stress_${t} hn_example_uppercase_selection)
  endif()
endforeach()
