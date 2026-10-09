add_executable(kura_cli main.cpp)
target_link_libraries(kura_cli PRIVATE kura_core)
target_compile_definitions(kura_cli PRIVATE KURA_VERSION_STRING="${PROJECT_VERSION}")
set_target_properties(kura_cli PROPERTIES OUTPUT_NAME "kura")
