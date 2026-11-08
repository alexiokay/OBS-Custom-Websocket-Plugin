cmake_minimum_required(VERSION 3.16...3.26)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(CTest)

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
  find_package(Qt6 COMPONENTS Widgets Core Network)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
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
        ${CMAKE_SOURCE_DIR}/other/mdns_cpp/src)

# Add all our source files
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE 
        obs_plugin/src/obs_plugin.cpp
        obs_plugin/src/obs_plugin.hpp
        obs_plugin/src/banner_manager.cpp
        obs_plugin/src/banner_manager.hpp
        obs_plugin/src/mdns_discovery.cpp
        obs_plugin/src/mdns_discovery.hpp
        obs_plugin/src/structured_mdns_query.cpp
        obs_plugin/src/structured_mdns_query.hpp
        obs_plugin/src/preview_feed.cpp
        obs_plugin/src/preview_feed.hpp
        obs_plugin/src/preview_feed_protocol.hpp
        obs_plugin/src/ui/service_selection_dialog.cpp
        obs_plugin/src/ui/service_selection_dialog.hpp
        obs_plugin/src/ui/ServiceSelectionDialog.ui
        obs_plugin/src/sources/overlay_source.cpp
        obs_plugin/src/sources/overlay_source.h
        obs_plugin/src/sources/banner_source.cpp
        obs_plugin/src/sources/banner_source.h
        obs_plugin/src/vortideck_common.h)

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

if(BUILD_TESTING)
  add_test(
    NAME overlay-capability-policy
    COMMAND
      ${CMAKE_COMMAND}
      -DOBS_PLUGIN_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/obs_plugin/src/obs_plugin.cpp
      -DOVERLAY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/obs_plugin/src/sources/overlay_source.cpp
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/overlay-capability-policy.cmake)

  add_executable(
    mdns-discovery-e2e
    cmake/tests/mdns-discovery-e2e.cpp
    cmake/tests/mdns-test-advertiser.cpp
    obs_plugin/src/mdns_discovery.cpp
    obs_plugin/src/structured_mdns_query.cpp)
  target_include_directories(
    mdns-discovery-e2e
    PRIVATE
      ${CMAKE_SOURCE_DIR}/obs_plugin/src
      ${CMAKE_SOURCE_DIR}/other/mdns_cpp/src)
  set_target_properties(
    mdns-discovery-e2e
    PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
  if(WIN32)
    target_link_libraries(mdns-discovery-e2e PRIVATE iphlpapi ws2_32)
  endif()
  add_test(NAME mdns-discovery-e2e COMMAND mdns-discovery-e2e)
  set_tests_properties(mdns-discovery-e2e PROPERTIES TIMEOUT 15)
  add_test(
    NAME mdns-structured-policy
    COMMAND
      ${CMAKE_COMMAND}
      -DMDNS_DISCOVERY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/obs_plugin/src/mdns_discovery.cpp
      -DSTRUCTURED_MDNS_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/obs_plugin/src/structured_mdns_query.cpp
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/mdns-structured-policy.cmake)
endif()

if(BUILD_TESTING)
  add_executable(preview-feed-contract cmake/tests/preview-feed-contract.cpp)
  target_include_directories(preview-feed-contract PRIVATE ${CMAKE_SOURCE_DIR}/obs_plugin/src)
  set_target_properties(preview-feed-contract PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME preview-feed-contract COMMAND preview-feed-contract)
endif()

if(BUILD_TESTING)
  add_executable(canvas-sync-contract cmake/tests/canvas-sync.cpp)
  target_include_directories(canvas-sync-contract PRIVATE ${CMAKE_SOURCE_DIR}/obs_plugin/src)
  target_link_libraries(canvas-sync-contract PRIVATE OBS::libobs)
  set_target_properties(canvas-sync-contract PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
  add_test(NAME canvas-sync-contract COMMAND canvas-sync-contract)
  set_tests_properties(canvas-sync-contract PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CMAKE_SOURCE_DIR}/.deps/bin/64bit")
  if(WIN32)
    # libobs also imports the bundled FFmpeg and compression runtimes.
    file(GLOB _obs_runtime_directories LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/.deps/obs-deps-*/bin")
    foreach(_obs_runtime_directory IN LISTS _obs_runtime_directories)
      set_property(TEST canvas-sync-contract APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_obs_runtime_directory}")
    endforeach()
  endif()
endif()

if(BUILD_TESTING)
  # Control-connection thread lifecycle under a VortiDeck that keeps going up
  # and down. `--legacy <scenario>` reproduces the pre-fix crashes (manual).
  add_executable(connection-lifecycle-stress cmake/tests/connection-lifecycle-stress.cpp)
  target_include_directories(
    connection-lifecycle-stress
    PRIVATE
      ${CMAKE_SOURCE_DIR}/obs_plugin/src
      ${CMAKE_SOURCE_DIR}/other/websocketpp
      ${CMAKE_SOURCE_DIR}/other/asio/asio/include)
  set_target_properties(connection-lifecycle-stress PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(WIN32)
    target_compile_definitions(connection-lifecycle-stress PRIVATE _WIN32_WINNT=0x0A00 _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING)
    target_link_libraries(connection-lifecycle-stress PRIVATE ws2_32 mswsock)
  endif()
  add_test(NAME connection-lifecycle-stress COMMAND connection-lifecycle-stress --seconds 8)
  set_tests_properties(connection-lifecycle-stress PROPERTIES TIMEOUT 60)
endif()
