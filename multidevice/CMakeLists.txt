find_package(PkgConfig REQUIRED)
pkg_check_modules(MULTIDEVICE_GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.52)
add_library(uxplay_multidevice STATIC supervisor.cpp worker.cpp)
target_link_libraries(uxplay_multidevice PUBLIC PkgConfig::MULTIDEVICE_GIO airplay)

option(BUILD_MULTIDEVICE_TESTS "Build multi-device transport integration tests" OFF)
if (BUILD_MULTIDEVICE_TESTS)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(multidevice_test_harness tests/supervisor_harness.cpp)
  target_link_libraries(multidevice_test_harness uxplay_multidevice)
  add_test(NAME multidevice_transport COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_supervisor.py
    $<TARGET_FILE:multidevice_test_harness>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_worker.py)
  set_tests_properties(multidevice_transport PROPERTIES TIMEOUT 90 ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()
