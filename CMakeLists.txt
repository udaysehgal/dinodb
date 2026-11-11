cmake_minimum_required(VERSION 3.16)
project(dinodb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -DDINODB_SANITIZE=thread (or address, undefined, "address,undefined")
set(DINODB_SANITIZE "" CACHE STRING "Sanitizers to build with")
if(DINODB_SANITIZE)
  add_compile_options(-fsanitize=${DINODB_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${DINODB_SANITIZE})
endif()

add_library(dinodb_core
  src/resp.cpp
  src/store.cpp
  src/commands.cpp
  src/persistence.cpp
  src/server.cpp
)
target_include_directories(dinodb_core PUBLIC include)
target_compile_options(dinodb_core PRIVATE -Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)
target_link_libraries(dinodb_core PUBLIC Threads::Threads)

add_executable(dinodb src/main.cpp)
target_link_libraries(dinodb PRIVATE dinodb_core)
target_compile_options(dinodb PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
