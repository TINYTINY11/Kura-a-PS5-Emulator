cmake_minimum_required(VERSION 3.21)
project(Kura VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Executables land in <build>/bin/<Config>/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(KURA_BUILD_TESTS "Build Kura unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(core)
add_subdirectory(frontend/cli)
add_subdirectory(frontend/boot) # guards itself: Windows-only GUI
add_subdirectory(tools)

if(KURA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/unit)
endif()
