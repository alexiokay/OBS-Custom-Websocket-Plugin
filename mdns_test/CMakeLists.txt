cmake_minimum_required(VERSION 3.16)
project(mdns_discovery_test)

# Require C++23 standard for newest features
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable additional warnings for better code quality
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Get the parent directory (where other/ folder is located)
get_filename_component(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)

# Create executable with mDNS discovery sources
add_executable(mdns_discovery_test
    # Main test file
    main.cpp
    # mDNS discovery implementation
    ../obs_plugin/src/mdns_discovery.cpp
    # mdns_cpp library sources
    ${DEPS_DIR}/other/mdns_cpp/src/logger.cpp
    ${DEPS_DIR}/other/mdns_cpp/src/mdns.cpp
    ${DEPS_DIR}/other/mdns_cpp/src/utils.cpp
)

# Set include directories
target_include_directories(mdns_discovery_test
    PRIVATE
    ../obs_plugin/src
    ${DEPS_DIR}/other/mdns_cpp/include
)

# Platform-specific settings
if(WIN32)
    # Add Windows-specific libraries
    target_link_libraries(mdns_discovery_test
        ws2_32
        iphlpapi
    )
    
    # Add Windows definitions
    target_compile_definitions(mdns_discovery_test PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

# Debug output
message(STATUS "DEPS_DIR: ${DEPS_DIR}")
message(STATUS "mdns_cpp include: ${DEPS_DIR}/other/mdns_cpp/include")