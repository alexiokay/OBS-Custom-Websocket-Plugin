cmake_minimum_required(VERSION 3.16...3.26)

project(vorti_obs_plugin VERSION 1.0.0)

# Options for Qt and frontend API
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

# Require C++23 standard for newest features
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add compile definitions
add_definitions(-D_KLOG_MODULE_NAME_=vorti_applets_obs_plugin)
add_definitions(-DUNICODE)
add_definitions(-D_UNICODE)
add_definitions(-DWIN32_LEAN_AND_MEAN)
add_definitions(-D_SCL_SECURE_NO_WARNINGS)

# Enable additional warnings for better code quality
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Get the parent directory to find dependencies
get_filename_component(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)

# Create the main plugin library as MODULE (following official template)
add_library(${CMAKE_PROJECT_NAME} MODULE)

# Try to find OBS libraries properly first, fallback to manual paths
if(WIN32)
    # Manual library setup for Windows (since we don't have proper OBS install)
    set(OBS_INCLUDE_DIRS 
        ${DEPS_DIR}/other/obs-studio
        ${DEPS_DIR}/other/obs-studio/libobs
        ${DEPS_DIR}/other/obs-studio/UI)
    
    set(OBS_LIBRARIES
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/x64/obs.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/x64/obs-frontend-api.lib
        ws2_32
        iphlpapi)
    
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${OBS_INCLUDE_DIRS})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${OBS_LIBRARIES})
else()
    # Use proper find_package on other platforms
    find_package(libobs REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
    
    if(ENABLE_FRONTEND_API)
        find_package(obs-frontend-api REQUIRED)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
    endif()
endif()

# Qt6 configuration (following official template)
if(ENABLE_QT)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
    
    # Set Qt properties (following official template)
    set_target_properties(${CMAKE_PROJECT_NAME}
        PROPERTIES 
            AUTOMOC ON
            AUTOUIC ON
            AUTORCC ON)
endif()

# Add include directories
target_include_directories(${CMAKE_PROJECT_NAME} 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/src
        ${DEPS_DIR}/other/websocketpp
        ${DEPS_DIR}/other/json/include
        ${DEPS_DIR}/other/asio/asio/include
        ${DEPS_DIR}/other/mdns_cpp/include)

# Add source files
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE 
        src/obs_plugin.cpp
        src/obs_plugin.hpp
        src/banner_manager.cpp
        src/banner_manager.hpp
        src/mdns_discovery.cpp
        src/mdns_discovery.hpp
        src/service_selection_dialog.cpp
        src/service_selection_dialog.hpp
        src/ServiceSelectionDialog.ui
        ../src/overlay_source.cpp
        ../src/overlay_source.h
        ../src/vortideck_common.h
        ${DEPS_DIR}/other/mdns_cpp/src/logger.cpp
        ${DEPS_DIR}/other/mdns_cpp/src/mdns.cpp
        ${DEPS_DIR}/other/mdns_cpp/src/utils.cpp)

# Set output name for the plugin
set_target_properties(${CMAKE_PROJECT_NAME} 
    PROPERTIES 
        OUTPUT_NAME "vorti_obs_plugin_x64"
        PREFIX "")

# Windows-specific configuration
if(WIN32)
    if(COMMAND generate_product_version)
        generate_product_version(VERSION
        PRODUCT_NAME "VortiDeck OBS Studio Plugin"
        PRODUCT_TYPE "VFT_DLL")
        target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${VERSION})
    endif()
endif()