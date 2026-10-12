add_executable(dinodb_tests
  test_store.cpp test_resp.cpp test_commands.cpp test_persistence.cpp test_concurrency.cpp test_main.cpp)
target_link_libraries(dinodb_tests PRIVATE dinodb_core)
add_test(NAME dinodb_tests COMMAND dinodb_tests)
