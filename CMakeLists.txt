cmake_minimum_required(VERSION 3.16)
project(dinodb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")

add_library(dinodb_core
  src/resp.cpp
  src/store.cpp
  src/commands.cpp
  src/persistence.cpp
  src/server.cpp
)
target_include_directories(dinodb_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(dinodb_core PUBLIC Threads::Threads)

add_executable(dinodb src/main.cpp)
target_link_libraries(dinodb PRIVATE dinodb_core)

enable_testing()
add_subdirectory(tests)
