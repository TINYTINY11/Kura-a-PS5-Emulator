add_executable(kura_tests test_log.cpp)
target_link_libraries(kura_tests PRIVATE kura_core)

add_executable(kura_net_tests test_net.cpp)
target_link_libraries(kura_net_tests PRIVATE kura_core)

add_executable(kura_pup_tests test_pup.cpp)
target_link_libraries(kura_pup_tests PRIVATE kura_firmware)

add_executable(kura_memory_tests test_memory.cpp)
target_link_libraries(kura_memory_tests PRIVATE kura_core)

add_executable(kura_elf_tests test_elf.cpp)
target_link_libraries(kura_elf_tests PRIVATE kura_core)

add_executable(kura_cpu_tests test_cpu.cpp)
target_link_libraries(kura_cpu_tests PRIVATE kura_core)

add_executable(kura_boot_tests test_boot.cpp)
target_link_libraries(kura_boot_tests PRIVATE kura_core)

add_executable(kura_settings_tests test_settings.cpp)
target_link_libraries(kura_settings_tests PRIVATE kura_core)

add_executable(kura_kernel_tests test_kernel.cpp)
target_link_libraries(kura_kernel_tests PRIVATE kura_core)

add_test(NAME unit.log COMMAND kura_tests)
add_test(NAME unit.net_filter COMMAND kura_net_tests)
add_test(NAME unit.pup COMMAND kura_pup_tests)
add_test(NAME unit.memory COMMAND kura_memory_tests)
add_test(NAME unit.elf COMMAND kura_elf_tests)
add_test(NAME unit.cpu COMMAND kura_cpu_tests)
add_test(NAME unit.boot COMMAND kura_boot_tests)
add_test(NAME unit.settings COMMAND kura_settings_tests)
add_test(NAME unit.kernel COMMAND kura_kernel_tests)
