cmake_minimum_required(VERSION 3.16)
project(camera_daemon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)
pkg_check_modules(LIBJPEG REQUIRED libjpeg)

# Optional: Find GStreamer for RTSP
pkg_check_modules(GSTREAMER gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${LIBCAMERA_INCLUDE_DIRS}
    ${LIBJPEG_INCLUDE_DIRS}
)

# Link directories
link_directories(
    ${LIBCAMERA_LIBRARY_DIRS}
    ${LIBJPEG_LIBRARY_DIRS}
)

# Core daemon library
add_library(camera_core STATIC
    src/camera/camera_manager.cpp
    src/camera/capture_pipeline.cpp
    src/camera/frame_rotator.cpp
    src/encoder/v4l2_encoder.cpp
    src/encoder/v4l2_jpeg.cpp
    src/buffer/ring_buffer.cpp
    src/buffer/raw_ring_buffer.cpp
    src/buffer/shared_memory.cpp
    src/ipc/control_socket.cpp
    src/ipc/frame_notifier.cpp
    src/capture/still_capture.cpp
    src/capture/clip_extractor.cpp
    src/streaming/rtsp_server.cpp
    src/streaming/v4l2_loopback.cpp
    src/utils/config.cpp
)

target_link_libraries(camera_core
    ${LIBCAMERA_LIBRARIES}
    ${LIBJPEG_LIBRARIES}
    pthread
    rt
)

if(GSTREAMER_FOUND)
    target_compile_definitions(camera_core PRIVATE HAVE_GSTREAMER)
    target_include_directories(camera_core PRIVATE ${GSTREAMER_INCLUDE_DIRS})
    target_link_libraries(camera_core ${GSTREAMER_LIBRARIES})
endif()

# Camera daemon executable
add_executable(ws-camerad
    src/main.cpp
)

target_link_libraries(ws-camerad
    camera_core
)

# Demo consumers
add_executable(still_client
    examples/still_client.cpp
)

add_executable(clip_client
    examples/clip_client.cpp
)

add_executable(frame_consumer
    examples/frame_consumer.cpp
)

target_link_libraries(frame_consumer rt pthread)

find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    add_executable(motion_clip
        examples/motion_clip.cpp
    )
    target_link_libraries(motion_clip rt pthread ${OpenCV_LIBS})
endif()

# GNUInstallDirs for proper FHS-compliant paths
include(GNUInstallDirs)

# Install daemon executable
install(TARGETS ws-camerad
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install example executables
set(_EXAMPLE_TARGETS still_client clip_client frame_consumer)
if(OpenCV_FOUND)
    list(APPEND _EXAMPLE_TARGETS motion_clip)
endif()
install(TARGETS ${_EXAMPLE_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install headers (for development)
install(DIRECTORY include/camera_daemon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install default config
install(FILES config/ws-camerad.conf
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/ws-camerad
)

# Install Python examples
install(DIRECTORY examples/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ws-camerad/examples
    FILES_MATCHING 
    PATTERN "*.py"
    PATTERN "*.cpp"
    PATTERN "__pycache__" EXCLUDE
)

# Create data directories
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_LOCALSTATEDIR}/lib/ws-camerad/stills)
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_LOCALSTATEDIR}/lib/ws-camerad/clips)

# Unit tests
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

