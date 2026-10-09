# kura_core — shared foundation (logging/tracing today; memory, loader, CPU... later)
add_library(kura_core STATIC
  common/log.cpp
  io/net_filter.cpp
)

target_include_directories(kura_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
