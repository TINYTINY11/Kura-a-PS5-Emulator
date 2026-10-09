add_executable(kura_tests test_log.cpp)
target_link_libraries(kura_tests PRIVATE kura_core)

add_executable(kura_net_tests test_net.cpp)
target_link_libraries(kura_net_tests PRIVATE kura_core)

add_test(NAME unit.log COMMAND kura_tests)
add_test(NAME unit.net_filter COMMAND kura_net_tests)
