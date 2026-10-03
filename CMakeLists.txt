cmake_minimum_required(VERSION 3.10)
project(MiDriverVR CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(
    ${CMAKE_SOURCE_DIR}/headers
    ${CMAKE_SOURCE_DIR}/MiDriverVR
)

find_package(OpenSSL REQUIRED)
find_package(X11 REQUIRED)

find_path(X264_INCLUDE_DIR x264.h)
find_library(X264_LIBRARY x264)

find_path(PULSE_INCLUDE_DIR pulse/simple.h)
find_library(PULSE_SIMPLE_LIBRARY pulse-simple)
find_library(PULSE_LIBRARY pulse)

set(SOURCES
    MiDriverVR/dllmain.cpp
    MiDriverVR/Usbtrackingserver.cpp
    MiDriverVR/Vrlowrespreview.cpp
)

add_library(driver_MiDriverVR SHARED ${SOURCES})

target_include_directories(driver_MiDriverVR PRIVATE
    ${OPENSSL_INCLUDE_DIR}
    ${X11_INCLUDE_DIR}
    ${X264_INCLUDE_DIR}
    ${PULSE_INCLUDE_DIR}
)

target_link_libraries(driver_MiDriverVR PRIVATE
    ${OPENSSL_LIBRARIES}
    ${X11_LIBRARIES}
    ${X264_LIBRARY}
    ${PULSE_SIMPLE_LIBRARY}
    ${PULSE_LIBRARY}
    pthread
)

set_target_properties(driver_MiDriverVR PROPERTIES
    PREFIX ""
    OUTPUT_NAME "driver_MiDriverVR"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/linux64"
)

# Also create alias driver_mi_driver_tutorial.so for manifest locator match if needed
add_custom_command(TARGET driver_MiDriverVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    driver_MiDriverVR.so
    "${CMAKE_SOURCE_DIR}/bin/linux64/driver_mi_driver_tutorial.so"
)
