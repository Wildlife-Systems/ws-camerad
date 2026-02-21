# CMake generated Testfile for 
# Source directory: /home/edwab/ws-camera-d/tests
# Build directory: /home/edwab/ws-camera-d/build/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
include("/home/edwab/ws-camera-d/build/tests/camera_daemon_tests[1]_include.cmake")
add_test(DaemonIntegrationTests "/home/edwab/ws-camera-d/build/tests/daemon_integration_tests")
set_tests_properties(DaemonIntegrationTests PROPERTIES  _BACKTRACE_TRIPLES "/home/edwab/ws-camera-d/tests/CMakeLists.txt;82;add_test;/home/edwab/ws-camera-d/tests/CMakeLists.txt;0;")
