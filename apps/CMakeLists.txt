cmake_minimum_required(VERSION 3.6)

project(libcamera-still)
add_executable(libcamera-still libcamera_still.cpp)
target_link_libraries(libcamera-still libcamera_app outputs images)

project(libcamera-hello)
add_executable(libcamera-hello libcamera_hello.cpp)
target_link_libraries(libcamera-hello libcamera_app)

set(EXECUTABLES libcamera-still libcamera-hello)

if (ENABLE_TFLITE)
    project(libcamera-detect)
    add_executable(libcamera-detect libcamera_detect.cpp)
    target_link_libraries(libcamera-detect libcamera_app images)
    set(EXECUTABLES ${EXECUTABLES} libcamera-detect)
endif()

set(EXECUTABLE_OUTPUT_PATH  ${CMAKE_BINARY_DIR})
install(TARGETS ${EXECUTABLES} RUNTIME DESTINATION bin)
