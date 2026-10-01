add_library(hn_platform STATIC
  instance_transport.cpp hyprland_ipc.cpp window_identity.cpp window_placement.cpp
  tray_controller.cpp autostart.cpp
  include/hn/platform/instance_transport.h include/hn/platform/hyprland_ipc.h
  include/hn/platform/tray_controller.h)
add_library(hn::platform ALIAS hn_platform)
target_include_directories(hn_platform PUBLIC include)
target_link_libraries(hn_platform PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# Tests live in tests/unit/platform_*.cpp (one executable each).
file(GLOB HN_PLATFORM_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/platform_test_*.cpp)
foreach(src ${HN_PLATFORM_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE hn_platform Qt6::Test)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endforeach()
# Helper process for the startup-race test.
add_executable(platform_race_helper ${CMAKE_SOURCE_DIR}/tests/unit/platform_race_helper.cpp)
target_link_libraries(platform_race_helper PRIVATE hn_platform)
