# Kura first-startup GUI (Windows-only, Win32 + GDI, no dependencies).
# The emulator's power-on frame: setup wizard + firmware-pipeline status.
if(NOT WIN32)
  return()
endif()

add_executable(kura_boot WIN32 main.cpp)
target_link_libraries(kura_boot PRIVATE kura_core dwmapi comdlg32)
set_target_properties(kura_boot PROPERTIES OUTPUT_NAME "kura_boot")
