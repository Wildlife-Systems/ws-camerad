# Unit tests for camera_daemon
find_package(GTest REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0)

# Unit test sources (no hardware required)
set(UNIT_TEST_SOURCES
    test_ring_buffer.cpp
    test_command.cpp
    test_common.cpp
    test_logger.cpp
    test_shared_memory.cpp
    test_config.cpp
    test_capture_structs.cpp
    test_control_socket.cpp
    test_integration.cpp
    test_v4l2_loopback.cpp
    test_frame_rotator.cpp
)

# Hardware test sources (need camera)
set(HARDWARE_TEST_SOURCES
    test_hardware.cpp
    test_full_integration.cpp
)

# Daemon integration tests (run against single daemon instance)
set(DAEMON_TEST_SOURCES
    test_daemon_integration.cpp
)

# Create main test executable (unit + hardware tests)
add_executable(camera_daemon_tests ${UNIT_TEST_SOURCES} ${HARDWARE_TEST_SOURCES})

target_include_directories(camera_daemon_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GTEST_INCLUDE_DIRS}
    ${LIBCAMERA_INCLUDE_DIRS}
)

target_link_libraries(camera_daemon_tests
    camera_core
    GTest::GTest
    GTest::Main
    ${LIBCAMERA_LIBRARIES}
    pthread
    rt
)

# Create separate daemon integration test executable
# These tests share ONE daemon instance and must run together
add_executable(daemon_integration_tests ${DAEMON_TEST_SOURCES})

target_include_directories(daemon_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GTEST_INCLUDE_DIRS}
    ${LIBCAMERA_INCLUDE_DIRS}
)

target_link_libraries(daemon_integration_tests
    camera_core
    GTest::GTest
    GTest::Main
    ${LIBCAMERA_LIBRARIES}
    pthread
    rt
)

# Match library's HAVE_GSTREAMER definition to avoid ODR violation
if(GSTREAMER_FOUND)
    target_compile_definitions(camera_daemon_tests PRIVATE HAVE_GSTREAMER)
    target_compile_definitions(daemon_integration_tests PRIVATE HAVE_GSTREAMER)
endif()

# Add unit and hardware tests using gtest_discover
include(GoogleTest)
gtest_discover_tests(camera_daemon_tests)

# Add daemon integration tests as a SINGLE test (runs all SharedDaemonTest.* together)
# This ensures the daemon starts once and all tests share it
add_test(NAME DaemonIntegrationTests COMMAND daemon_integration_tests)
