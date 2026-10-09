# kura_core — shared foundation (logging, net filter, guest memory, ELF loader, CPU interpreter, kernel HLE)
add_library(kura_core STATIC
  common/log.cpp
  common/settings.cpp
  io/net_filter.cpp
  memory/guest_memory.cpp
  loader/elf.cpp
  cpu/interpreter.cpp
  kernel/vfs.cpp
  kernel/dynlib.cpp
  kernel/kernel.cpp
)

target_include_directories(kura_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
