add_executable(hyprnotes_live_driver qt_driver.cpp)
target_link_libraries(hyprnotes_live_driver PRIVATE hn_app Qt6::Test)
