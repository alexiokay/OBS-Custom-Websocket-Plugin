add_definitions(-D_KLOG_MODULE_NAME_=vorti_applets_obs_plugin)
add_definitions(-DUNICODE)
add_definitions(-D_UNICODE)
add_definitions(-DWIN32_LEAN_AND_MEAN)
add_definitions(-D_SCL_SECURE_NO_WARNINGS)
if(WIN32)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /DEBUG")
endif()

# Require C++23 standard for newest features
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable additional warnings for better code quality (but don't treat warnings as errors)
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

set(PLUGIN_NAME "vorti_obs")
set(APPLET_NAME "${PLUGIN_NAME}_plugin")
set(BUNDLE_NAME "${PLUGIN_NAME}.plugin")

set(SOURCES
    src/obs_plugin.cpp
    src/banner_manager.cpp)

set(HEADERS
    src/obs_plugin.hpp
    src/banner_manager.hpp)

if(WIN32)
    if("${CMAKE_SIZEOF_VOID_P}" EQUAL "8") 
        set(LIBS 
            ${CMAKE_CURRENT_SOURCE_DIR}/libs/x64/obs.lib
            ${CMAKE_CURRENT_SOURCE_DIR}/libs/x64/obs-frontend-api.lib)
    endif()

    if(COMMAND generate_product_version)
        generate_product_version(VERSION
        PRODUCT_NAME "VortiDeck OBS Studio Plugin"
        PRODUCT_TYPE "VFT_DLL")
    endif()
    add_library(vorti_applets_obs_plugin SHARED ${SOURCES} ${HEADERS} ${VERSION})
elseif(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
    set(LIBS 
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/osx/libobs.dylib
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/osx/obs-frontend-api.dylib)

    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--export-all-symbols")

    add_library(vorti_applets_obs_plugin MODULE ${SOURCES} ${HEADERS} ${VERSION})

endif()


if(WIN32)
    # Rename the binary based on the bitness
    if("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
        set_target_properties(vorti_applets_obs_plugin PROPERTIES OUTPUT_NAME "${APPLET_NAME}_x64")
    endif()
elseif(APPLE)
    set_target_properties(vorti_applets_obs_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/vorti/applets/ghub-obs-plugin/${BUNDLE_NAME}/Contents/MacOS
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/vorti/applets/ghub-obs-plugin/${BUNDLE_NAME}/Contents/MacOS
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/vorti/applets/ghub-obs-plugin/${BUNDLE_NAME}/Contents/MacOS
        OUTPUT_NAME "${PLUGIN_NAME}"
        PREFIX ""
        SUFFIX ""
    )
else()
    # This will be used for Linux and others  
    # Try to find OBS development files first
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBOBS QUIET libobs)
    endif()
    
    if(LIBOBS_FOUND)
        # Use pkg-config found libraries
        set(LIBS ${LIBOBS_LIBRARIES})
        add_library(vorti_applets_obs_plugin SHARED ${SOURCES} ${HEADERS} ${VERSION})
        target_include_directories(vorti_applets_obs_plugin PRIVATE ${LIBOBS_INCLUDE_DIRS})
        target_compile_options(vorti_applets_obs_plugin PRIVATE ${LIBOBS_CFLAGS_OTHER})
    else()
        # Fallback: look for system libraries manually
        find_library(OBS_LIB NAMES obs libobs PATHS /usr/lib /usr/lib/x86_64-linux-gnu)
        find_path(OBS_INCLUDE_DIR NAMES obs/obs.h PATHS /usr/include)
        
        if(OBS_LIB AND OBS_INCLUDE_DIR)
            set(LIBS ${OBS_LIB})
            add_library(vorti_applets_obs_plugin SHARED ${SOURCES} ${HEADERS} ${VERSION})
            target_include_directories(vorti_applets_obs_plugin PRIVATE ${OBS_INCLUDE_DIR})
        else()
            # Build without linking to OBS libraries (plugin will use runtime linking)
            message(WARNING "OBS development libraries not found. Building without explicit linking.")
            add_library(vorti_applets_obs_plugin SHARED ${SOURCES} ${HEADERS} ${VERSION})
        endif()
    endif()
endif()

get_filename_component(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)

# Debug output to help diagnose path issues
message(STATUS "Current source dir: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "DEPS_DIR: ${DEPS_DIR}")
message(STATUS "JSON include path: ${DEPS_DIR}/other/json/include")
message(STATUS "OBS include path: ${DEPS_DIR}/other/obs-studio")

target_include_directories(vorti_applets_obs_plugin
    PUBLIC
    include)

# Set include directories with platform-specific handling
if(WIN32)
    target_include_directories(vorti_applets_obs_plugin
        PRIVATE
        ${DEPS_DIR}/other/obs-studio
        ${DEPS_DIR}/other/obs-studio/libobs
        ${DEPS_DIR}/other/obs-studio/UI
        ${DEPS_DIR}/other/websocketpp
        ${DEPS_DIR}/other/json/include
        ${DEPS_DIR}/other/asio/asio/include)
else()
    target_include_directories(vorti_applets_obs_plugin
        SYSTEM PUBLIC
        ${DEPS_DIR}/other/obs-studio
        ${DEPS_DIR}/other/websocketpp
        ${DEPS_DIR}/other/json/include
        ${DEPS_DIR}/other/asio/asio/include)
endif()

target_link_libraries(vorti_applets_obs_plugin
    ${LIBS})

if(LGHUB_BACKEND_TARGET)
    configure_warning_suppression(vorti_applets_obs_plugin PRIVATE)
endif()
