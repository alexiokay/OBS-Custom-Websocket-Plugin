cmake_minimum_required(VERSION 3.16...3.26)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME} PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
                                  -Wno-comma>)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON
               AUTOUIC ON
               AUTORCC ON)
endif()

# Add our source directory
target_include_directories(${CMAKE_PROJECT_NAME} 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/obs_plugin/src
        ${CMAKE_SOURCE_DIR}/other/websocketpp
        ${CMAKE_SOURCE_DIR}/other/json/include
        ${CMAKE_SOURCE_DIR}/other/asio/asio/include
        ${CMAKE_SOURCE_DIR}/other/mdns_cpp/include)

# Add all our source files
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE 
        obs_plugin/src/obs_plugin.cpp
        obs_plugin/src/obs_plugin.hpp
        obs_plugin/src/banner_manager.cpp
        obs_plugin/src/banner_manager.hpp
        obs_plugin/src/mdns_discovery.cpp
        obs_plugin/src/mdns_discovery.hpp
        obs_plugin/src/service_selection_dialog.cpp
        obs_plugin/src/service_selection_dialog.hpp
        obs_plugin/src/ServiceSelectionDialog.ui
        other/mdns_cpp/src/logger.cpp
        other/mdns_cpp/src/mdns.cpp
        other/mdns_cpp/src/utils.cpp)

# Add compile definitions
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    _KLOG_MODULE_NAME_=vorti_applets_obs_plugin
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _SCL_SECURE_NO_WARNINGS
    _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING)

# Disable warnings as errors and problematic warnings
if(MSVC)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE 
        /WX-      # Disable warnings as errors
        /wd4267   # Disable size_t conversion warnings
        /wd4244   # Disable int conversion warnings
    )
    # Also set globally
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /WX- /wd4267 /wd4244")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /WX- /wd4267 /wd4244")
endif()

# Set C++23 standard
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})