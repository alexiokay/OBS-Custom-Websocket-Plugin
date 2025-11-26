#include "obs_plugin.hpp"
#include "ui/service_selection_dialog.hpp"
#include "sources/overlay_source.h"
#include "vortideck_common.h"

// Forward declarations for source registrations
extern void register_banner_source();
extern void register_overlay_source();
#include <QApplication>
#include <QMetaObject>
#include <QThread>
#include <QMenuBar>
#include <QMainWindow>
#include <QAction>
#include <QMenu>

#define _WEBSOCKETPP_CPP11_RANDOM_DEVICE_

#ifndef ASIO_STANDALONE
    #define ASIO_STANDALONE
#endif

#ifndef _WEBSOCKETPP_CPP11_TYPE_TRAITS_
    #define _WEBSOCKETPP_CPP11_TYPE_TRAITS_
#endif

#pragma warning(push)
#pragma warning(disable : 4267)
#include <nlohmann/json.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#pragma warning(pop)

#include <obs.h>
#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/platform.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif
#include <sstream>

using namespace vorti::applets::obs_plugin;

// Forward declaration
void log_to_obs(std::string_view message);

// Note: Since obs_plugin is a namespace, we don't need a global instance pointer

// Global function to access banner_manager from banner_source
vorti::applets::banner_manager& get_global_banner_manager()
{
    return vorti::applets::obs_plugin::m_banner_manager;
}

// Global function to get WebSocket connection URL for banner defaults
std::string get_global_websocket_url()
{
    try {
        // Use the selected service URL if available
        if (!m_selected_service_url.empty()) {
            return m_selected_service_url;
        }
        // Fallback to discovered URL
        if (!m_discovered_websocket_url.empty()) {
            return m_discovered_websocket_url;
        }
        // Call get_connection_url function
        return get_connection_url();
    } catch (...) {
        return "https://vortideck.com"; // Fallback if not connected
    }
}

// Function to update all existing overlay URLs when connected to a new service
void update_all_overlay_urls_to_connected_server()
{
    std::string websocket_url = get_global_websocket_url();
    if (websocket_url == "https://vortideck.com") {
        return; // Not connected to a real service
    }
    
    // Build the overlay URL
    std::string overlay_url;
    if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
        std::string base_url;
        if (websocket_url.starts_with("ws://")) {
            base_url = "http://" + websocket_url.substr(5);
        } else {
            base_url = "https://" + websocket_url.substr(6);
        }
        // Remove /ws path if present since that's WebSocket-only
        if (base_url.ends_with("/ws")) {
            base_url = base_url.substr(0, base_url.length() - 3);
        }
        // Ensure no double slashes
        if (base_url.ends_with("/")) {
            overlay_url = base_url + "overlay.html";
        } else {
            overlay_url = base_url + "/overlay.html";
        }
    } else {
        overlay_url = websocket_url + "/overlay.html";
    }
    
    // Update all existing overlay sources
    obs_enum_sources([](void* data, obs_source_t* source) {
        std::string url = *(std::string*)data;
        
        // Check if this is a VortiDeck overlay source
        const char* source_id = obs_source_get_id(source);
        if (source_id && strcmp(source_id, vortideck::SOURCE_ID_OVERLAY) == 0) {
            // Update the overlay URL
            obs_data_t* settings = obs_source_get_settings(source);
            obs_data_set_string(settings, "url", url.c_str());
            obs_source_update(source, settings);
            obs_data_release(settings);
            
            const char* name = obs_source_get_name(source);
            blog(LOG_INFO, "Auto-updated overlay '%s' to connected server: %s", 
                 name ? name : "unnamed", url.c_str());
        }
        return true; // Continue enumeration
    }, &overlay_url);
    
    log_to_obs("Auto-updated all overlay URLs to connected server: " + overlay_url);
}

// Function to update all existing banner URLs when connected to a new service
void update_all_banner_urls_to_connected_server()
{
    std::string websocket_url = get_global_websocket_url();
    if (websocket_url == "https://vortideck.com") {
        return; // Not connected to a real service
    }
    
    // Build the banner URL (identical logic to overlays)
    std::string banner_url;
    if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
        std::string base_url;
        if (websocket_url.starts_with("ws://")) {
            base_url = "http://" + websocket_url.substr(5);
        } else {
            base_url = "https://" + websocket_url.substr(6);
        }
        // Remove /ws path if present since that's WebSocket-only
        if (base_url.ends_with("/ws")) {
            base_url = base_url.substr(0, base_url.length() - 3);
        }
        // Ensure no double slashes
        if (base_url.ends_with("/")) {
            banner_url = base_url + "banners";
        } else {
            banner_url = base_url + "/banners";
        }
    } else {
        banner_url = websocket_url + "/banners";
    }
    
    // Update the banner_manager with the new URL
    if constexpr (BANNER_MANAGER_ENABLED) {
        try {
            auto& banner_mgr = get_global_banner_manager();
            banner_mgr.set_banner_url(banner_url);
            log_to_obs("Updated banner manager to use connected server: " + banner_url);
        } catch (...) {
            log_to_obs("Failed to update banner manager URL");
        }
    }
}



// Simple plugin interface for banner manager
struct plugin_interface {
    bool is_connected() const;
    bool send_message(const nlohmann::json& message) const;
    void show_connection_settings_dialog() const;
};

// Implementation of plugin_interface methods
bool plugin_interface::is_connected() const {
    return vorti::applets::obs_plugin::is_connected();
}

bool plugin_interface::send_message(const nlohmann::json& message) const {
    return vorti::applets::obs_plugin::send_message(message);
}

void plugin_interface::show_connection_settings_dialog() const {
    vorti::applets::obs_plugin::show_connection_settings_dialog();
}

// Global plugin instance for cross-module access
plugin_interface* g_obs_plugin_instance = new plugin_interface();

// Log function definition (needed before other functions use it)
void log_to_obs(std::string_view message) {
    blog(LOG_INFO, "%s", message.data());  // string_view version
    // Use regular logging for now
    printf("[OBS Plugin] %.*s\n", static_cast<int>(message.size()), message.data());
}

/*
Helpful resources: https://obsproject.com/docs/reference-modules.html
*/

/* Defines common functions (required) */
// TEMPORARILY COMMENT OUT OBS FUNCTIONS
OBS_DECLARE_MODULE()  // Required for OBS to recognize the plugin

const char *obs_module_name()
{
    // The full name of the module
    return s_integration_name.c_str();
}

const char *obs_module_description()
{
    // A description of the module
    return s_integration_description.c_str();
}

bool obs_module_load()
{
    // Called when the module is loaded.
    m_shutting_down = false;
    
    // obs_plugin is a namespace, so no instance to set
    
    blog(LOG_INFO, "VortiDeck OBS Plugin loaded successfully");
    return true;
}

static void handle_obs_frontend_save(obs_data_t * /*save_data*/, bool /*saving*/, void *)
{
    // Save implies frontend loaded
    // Populate our collections map here
    if (helper_populate_collections())
    {
        register_parameter_actions();
    }
}

static void handle_obs_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP)
    {
        m_collection_locked = true;

        // CRITICAL: Don't clean up banner sources if we're shutting down - OBS will handle it
        // This prevents banner restoration threads from spawning during shutdown
        if constexpr (BANNER_MANAGER_ENABLED) {
            if (!m_shutting_down) {
                m_banner_manager.cleanup_for_scene_collection_change();
            } else {
                blog(LOG_INFO, "[OBS Plugin] Skipping banner cleanup - shutdown in progress");
            }
        }
    }
    else if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED
             || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED || event == OBS_FRONTEND_EVENT_SCENE_CHANGED)
    {
        m_collection_locked = false;
        std::lock_guard<std::mutex> wlock(m_thread_lock);
        if (helper_populate_collections())
        {
            register_parameter_actions();
        }
        
        // Re-initialize banners after scene collection changes are complete
        if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
            if constexpr (BANNER_MANAGER_ENABLED) {
                // Use printf since log_to_obs is not available in this scope
                printf("[OBS Plugin] Scene collection changed - re-initializing banners\n");
                m_banner_manager.initialize_after_obs_ready();

                // SAFE: Enable signal connections after banners are stable
                printf("[OBS Plugin] Enabling banner signal protection after scene collection change\n");
                m_banner_manager.enable_signal_connections_when_safe();
            }
        }

        // SAFE: Enable signal connections when scenes are first available
        if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) {
            if constexpr (BANNER_MANAGER_ENABLED) {
                static bool signals_enabled = false;
                if (!signals_enabled) {
                    printf("[OBS Plugin] Scene list ready - enabling banner signal protection\n");
                    m_banner_manager.enable_signal_connections_when_safe();
                    signals_enabled = true;
                }
            }
        }
    }
    else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED || event == OBS_FRONTEND_EVENT_RECORDING_STARTED)
    {
        std::lock_guard<std::mutex> wlock(m_thread_lock);
        m_start_time = std::chrono::high_resolution_clock::now();
        m_total_streamed_bytes = 0;
        m_total_streamed_frames = 0;
    }
    else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED || event == OBS_FRONTEND_EVENT_RECORDING_STOPPED)
    {
        std::lock_guard<std::mutex> wlock(m_thread_lock);
        m_total_streamed_bytes = 0;
        m_total_streamed_frames = 0;
    }
    else if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED)
    {
        std::lock_guard<std::mutex> wlock(m_thread_lock);
        m_studio_mode = true;
    }
    else if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED)
    {
        std::lock_guard<std::mutex> wlock(m_thread_lock);
        m_studio_mode = false;
    }
    else if (event == OBS_FRONTEND_EVENT_EXIT)
    {
        blog(LOG_INFO, "[OBS Plugin] EXIT event received - performing shutdown");
        m_shutting_down = true;

        // CRITICAL: Set banner_manager shutdown flag IMMEDIATELY to stop signal handlers
        // Sources are destroyed shortly after this event, before obs_module_unload()
        if constexpr (BANNER_MANAGER_ENABLED) {
            blog(LOG_INFO, "[OBS Plugin] Setting banner_manager shutdown flag in EXIT event");
            m_banner_manager.set_shutting_down();
        }

        // CRITICAL: Stop websocket immediately to interrupt any pending connection attempts
        // Only call stop() if websocket thread was started (which calls init_asio)
        if (m_websocket_thread) {
            try {
                std::lock_guard<std::mutex> wl(m_lock);
                m_websocket.stop();
                blog(LOG_INFO, "[OBS Plugin] Websocket stopped");
            } catch (const std::exception& e) {
                blog(LOG_WARNING, "[OBS Plugin] Exception stopping websocket: %s", e.what());
            } catch (...) {
                blog(LOG_WARNING, "[OBS Plugin] Unknown exception stopping websocket");
            }
        }

        // Notify any waiting condition variables to wake up and check shutdown flag
        m_compressor_ready_cv.notify_all();

        // CRITICAL: Stop mDNS discovery FIRST before it tries to reconnect
        // The discovery thread can still be running and accessing plugin memory
        stop_continuous_discovery();

        // Shutdown banner manager
        if constexpr (BANNER_MANAGER_ENABLED) {
            blog(LOG_INFO, "[OBS Plugin] Shutting down banner manager");
            m_banner_manager.shutdown();
        }

        // DON'T stop websocket loop or uninitialize - process will terminate naturally
        blog(LOG_INFO, "[OBS Plugin] EXIT event complete");
    }
}


void obs_module_post_load()
{
    register_integration();

    // Register all available actions
    initialize_actions();
    
    // Start connection in a separate jthread to avoid blocking OBS startup
    std::jthread([]{
        // Add a small delay to let OBS finish initializing
        std::this_thread::sleep_for(std::chrono::seconds(1));
        connect();
    }).detach();
    register_regular_actions();

    obs_frontend_add_event_callback(handle_obs_frontend_event, nullptr);
    obs_frontend_add_save_callback(handle_obs_frontend_save, nullptr);
    
    // Connect to video reset signals for canvas size sync
    connect_video_reset_signals();

    // Register custom VortiDeck sources
    if constexpr (BANNER_MANAGER_ENABLED) {
        register_banner_source();   // Banner source for free users
    }
    if constexpr (OVERLAY_ENABLED) {
        register_overlay_source();  // Overlay source
    }

    // Initialize VortiDeck menu
    if constexpr (VORTIDECK_MENU_ENABLED) {
        create_obs_menu();
    }

    register_actions_broadcast();

    start_loop();
}

void obs_module_unload()
{
    // Called when the module is unloaded.
    // NOTE: Most cleanup already happened in OBS_FRONTEND_EVENT_EXIT
    // Keep this minimal to avoid triggering OBS core audio crash bug
    blog(LOG_INFO, "[OBS Plugin] OBS module unloading - minimal cleanup");

    // CRITICAL: Set banner_manager shutdown flag FIRST before ANY cleanup
    // This stops signal handlers immediately, even if they're already queued
    if constexpr (BANNER_MANAGER_ENABLED) {
        blog(LOG_INFO, "[OBS Plugin] Setting banner_manager shutdown flag IMMEDIATELY");
        vorti::applets::obs_plugin::m_banner_manager.set_shutting_down();
        blog(LOG_INFO, "[OBS Plugin] Disconnecting banner_manager signals");
        vorti::applets::obs_plugin::m_banner_manager.disconnect_all_signals();
    }

    // Just set flag - everything else already cleaned up in EXIT event
    {
        std::lock_guard<std::mutex> wl(vorti::applets::obs_plugin::m_lock);
        vorti::applets::obs_plugin::m_shutting_down = true;
    }

    // CRITICAL: Immediately detach threads - DO NOT SLEEP during shutdown
    // Sleeping blocks OBS shutdown and causes WASAPI/CEF crashes as they're torn down
    // Threads have m_shutting_down flag set and will exit gracefully on their own

    // Detach websocket thread immediately
    if (vorti::applets::obs_plugin::m_websocket_thread &&
        vorti::applets::obs_plugin::m_websocket_thread->joinable()) {
        blog(LOG_INFO, "[OBS Plugin] Detaching websocket thread (will exit via shutdown flag)");
        vorti::applets::obs_plugin::m_websocket_thread->detach();
    }

    // Detach mDNS discovery thread immediately
    if (vorti::applets::obs_plugin::m_continuous_discovery_thread.joinable()) {
        blog(LOG_INFO, "[OBS Plugin] Detaching mDNS discovery thread (will exit via shutdown flag)");
        vorti::applets::obs_plugin::m_continuous_discovery_thread.detach();
    }

    blog(LOG_INFO, "[OBS Plugin] OBS module unload complete");
}

bool vorti::applets::obs_plugin::connect()
{
    if (m_shutting_down) {
        log_to_obs("Not connecting - shutting down");
        return false;
    }

    if (is_connected())
    {
        log_to_obs("Already connected");
        return false;
    }

    log_to_obs("Starting connection process...");

    // Start continuous mDNS discovery if enabled
    if (m_use_mdns_discovery) {
        log_to_obs("Starting continuous mDNS discovery for VortiDeck services...");
        start_continuous_discovery();
        
        // Try to load last known service first
        if (load_last_known_service_state()) {
            log_to_obs("Using last known VortiDeck service for connection");
        } else {
            log_to_obs("No previous service found, waiting for discovery...");
            // Give discovery a few seconds to find a service
            auto discovery_start = std::chrono::steady_clock::now();
            while (!m_service_found.load() && !m_shutting_down && 
                   (std::chrono::steady_clock::now() - discovery_start) < std::chrono::seconds(5)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    } else {
        log_to_obs("mDNS discovery disabled, using fallback connection");
    }

    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
        m_current_message_id = 1;
        m_integration_guid.clear();
        m_integration_instance.clear();

        if (nullptr == m_websocket_thread)
        {
            m_websocket_thread.reset(new std::jthread(&vorti::applets::obs_plugin::_run_forever));
        }
    }

    // Wait for connection with timeout, checking shutdown flag periodically
    {
        std::unique_lock<std::mutex> lk(m_compressor_ready_mutex);
        auto start_time = std::chrono::system_clock::now();
        while (!m_websocket_open && !m_shutting_down) {
            // Wait for 100ms at a time to check shutdown flag frequently
            if (m_compressor_ready_cv.wait_for(lk, std::chrono::milliseconds(100)) == std::cv_status::timeout) {
                // Check if total timeout exceeded
                if (std::chrono::system_clock::now() - start_time > std::chrono::seconds(3)) {
                    log_to_obs("Connection timeout - will retry later");
                    disconnect();  // Clean up current attempt, but don't set m_shutting_down
                    return false;  // This will trigger reconnection logic in _run_forever
                }
            }
        }

        // If we exited because of shutdown, don't proceed
        if (m_shutting_down) {
            log_to_obs("Connection wait interrupted by shutdown");
            return false;
        }
    }

    // Start initialization sequence
    log_to_obs("Connection established, starting initialization sequence...");

    // CRITICAL: Check shutdown flag again before initializing
    if (m_shutting_down) {
        log_to_obs("Aborting initialization - shutdown in progress");
        return false;
    }

    // Reset failure counter on successful connection
    m_connection_failure_count = 0;

    // Registration will be handled by websocket_open_handler() when connection is truly open


    log_to_obs("Initialization sequence completed successfully");
    return true;
}


// infinite reconnect
void vorti::applets::obs_plugin::reconnect() {
    if (m_shutting_down) {
        log_to_obs("Not attempting reconnection - shutting down");
        return;
    }

    log_to_obs("Starting reconnection sequence...");
    
    // First, properly clean up existing connection
    disconnect();

    int attempt = 0;
    const int retry_delay_seconds = 2;
    
    while (!is_connected() && !m_shutting_down) {
        try {
            log_to_obs(std::format("Reconnection attempt {}", attempt + 1));
            
            // Attempt to establish new connection with full initialization
            if (connect()) {
                log_to_obs("Reconnection successful");
                return;
            }
            
            attempt++;
            // Interruptible sleep - check shutdown every 100ms
            for (int i = 0; i < retry_delay_seconds * 10 && !m_shutting_down; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

        } catch (const std::exception& e) {
            log_to_obs("Reconnection attempt failed: " + std::string(e.what()));
            attempt++;
            // Interruptible sleep - check shutdown every 100ms  
            for (int i = 0; i < retry_delay_seconds * 10 && !m_shutting_down; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}


void vorti::applets::obs_plugin::websocket_close_handler(const websocketpp::connection_hdl &connection_handle)
{
    if (m_shutting_down) {
        log_to_obs("Connection closed during shutdown");
        return;
    }

    bool should_reconnect = false;
    {
        std::lock_guard<std::mutex> wl(m_lock);
        if (m_websocket_open) {
            m_websocket_open = false;
            m_integration_guid.clear();
            m_integration_instance.clear();
            m_current_message_id = 1;
            should_reconnect = true;
            log_to_obs("Connection closed unexpectedly, will attempt reconnect");
        }
    }

    // Notify any waiting jthreads
    m_compressor_ready_cv.notify_all();
    m_initialization_cv.notify_all();

    // Don't create detached threads during shutdown - causes crashes
    // Reconnection will be handled by the main websocket loop instead
    if (should_reconnect && !m_shutting_down) {
        log_to_obs("Connection lost - reconnection will be attempted by main loop");
    }
}


void vorti::applets::obs_plugin::disconnect()
{
    log_to_obs("Disconnect: Starting connection cleanup");
    
    // Set connection state first
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
        m_integration_guid.clear();
        m_integration_instance.clear();
        m_current_message_id = 1;
        // DON'T set m_shutting_down = true here - only set it during actual shutdown
        // This allows reconnection attempts to continue
    }
    
    // Keep continuous discovery running during disconnects (not shutdown)
    // This allows it to find new services while we're disconnected

    // Notify all waiting threads immediately
    m_compressor_ready_cv.notify_all();
    m_initialization_cv.notify_all();

    // Stop the status update loop first
    stop_loop();

    // FIXED: Improved websocket shutdown with timeout to prevent hangs
    if (m_websocket_thread)
    {
        try {
            log_to_obs("Disconnect: Stopping websocket");

            // CRITICAL: During shutdown, don't block - just detach and exit fast
            if (m_shutting_down) {
                log_to_obs("Disconnect: Shutdown in progress - detaching websocket thread");
                if (m_websocket_thread->joinable()) {
                    m_websocket_thread->request_stop();
                    m_websocket_thread->detach();
                }
                m_websocket_thread.reset();
                log_to_obs("Disconnect: Fast shutdown complete");
                return;  // Exit immediately, don't block
            }

            // Stop the websocket gracefully
            m_websocket.stop();

            // Wait for jthread to finish with timeout to prevent hanging
            if (m_websocket_thread->joinable())
            {
                log_to_obs("Disconnect: Requesting thread stop");
                m_websocket_thread->request_stop();
                
                // Use a separate thread to join with timeout
                std::atomic<bool> join_completed{false};
                std::jthread timeout_thread([&]() {
                    m_websocket_thread->join();
                    join_completed = true;
                });
                
                // Wait up to 3 seconds for graceful shutdown
                auto timeout_start = std::chrono::steady_clock::now();
                const auto timeout_duration = std::chrono::seconds(3);
                
                while (!join_completed && 
                       (std::chrono::steady_clock::now() - timeout_start) < timeout_duration) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                
                if (!join_completed) {
                    log_to_obs("Disconnect: Thread join timeout - forcing cleanup");
                    // Thread is stuck, just reset and let it die
                    timeout_thread.detach();
                } else {
                    log_to_obs("Disconnect: Thread joined successfully");
                }
            }
            
            // Reset the websocket client state
            m_websocket.reset();
            
        } catch (const std::exception& e) {
            log_to_obs("Error during WebSocket shutdown: " + std::string(e.what()));
        }
        
        // Reset the thread - at this point we've either joined or timed out
        m_websocket_thread.reset();
        log_to_obs("Disconnect: Websocket thread cleanup complete");
    }

    log_to_obs("Disconnect: Connection cleanup complete");
}


bool vorti::applets::obs_plugin::is_connected()
{
    std::lock_guard<std::mutex> rl(m_lock);
    return m_websocket_open;
}


void vorti::applets::obs_plugin::_run_forever()
{
    // Initial setup
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket.init_asio();
        m_websocket.clear_access_channels(websocketpp::log::alevel::all);
        m_websocket.set_access_channels(websocketpp::log::alevel::connect);
        m_websocket.set_access_channels(websocketpp::log::alevel::disconnect);
        m_websocket.set_access_channels(websocketpp::log::alevel::app);
        
        // Set a reasonable reuse address to prevent hanging
        m_websocket.set_reuse_addr(true);
    }

    while (!m_shutting_down)
    {
        try
        {
            websocketpp::lib::error_code ec;
            ws_client::connection_ptr connection = _create_connection();
            
            if (!connection) {
                int failure_count = ++m_connection_failure_count;
                log_to_obs(std::format("Failed to create connection - retrying in 1 second (failure #{}/10)", failure_count));
                
                // After 10 failures, reset dialog flag to allow service selection again
                if (failure_count >= 10) {
                    log_to_obs("Multiple connection failures - allowing service selection dialog again");
                    m_show_selection_dialog = false;
                    m_connection_failure_count = 0; // Reset counter
                }
                
                // Interruptible sleep - check shutdown every 100ms
                for (int i = 0; i < 10 && !m_shutting_down; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            // Connect and run
            m_websocket.connect(connection);
            
            try {
                // Run websocket with periodic shutdown checks
                m_websocket.run();
            } catch (const std::exception& e) {
                log_to_obs("Exception in WebSocket run: " + std::string(e.what()));
            }
            
            // Additional shutdown check after websocket.run() exits
            if (m_shutting_down) {
                log_to_obs("WebSocket run exited due to shutdown");
                break;
            }
            
            // If we get here, the connection was closed
            {
                std::lock_guard<std::mutex> wl(m_lock);
                if (m_websocket_open) {
                    m_websocket_open = false;
                }
            }
            
            log_to_obs("Connection lost - attempting to reconnect in 1 second");

            // Reset for next attempt (skip if shutting down to avoid blocking)
            if (!m_shutting_down) {
                m_websocket.reset();
            }

            // Small delay before next attempt - interruptible
            for (int i = 0; i < 10 && !m_shutting_down; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        catch (const std::exception& e)
        {
            log_to_obs("Exception in WebSocket loop: " + std::string(e.what()));
            
            // Reset connection state
            {
                std::lock_guard<std::mutex> wl(m_lock);
                m_websocket_open = false;
            }

            // Reset websocket (skip if shutting down to avoid blocking)
            if (!m_shutting_down) {
                m_websocket.reset();
            }

            // Interruptible sleep - check shutdown every 100ms
            for (int i = 0; i < 10 && !m_shutting_down; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}


ws_client::connection_ptr vorti::applets::obs_plugin::_create_connection()
{
    // CRITICAL: Check shutdown flag immediately
    if (m_shutting_down) {
        return nullptr;
    }

    websocketpp::lib::error_code error_code;

    // Get the connection URL from mDNS discovery or fallback
    std::string connection_url = get_connection_url();
    log_to_obs(std::format("Connecting to: {}", connection_url).c_str());
    
    auto connection = m_websocket.get_connection(connection_url, error_code);

    assert(connection || 0 != error_code.value());
    if (!connection || 0 != error_code.value())
    {
        return connection;
    }

    connection->set_open_handler([](const websocketpp::connection_hdl &connection_handle) {
        vorti::applets::obs_plugin::websocket_open_handler(connection_handle);
    });
    connection->set_message_handler(
        [](const websocketpp::connection_hdl &connection_handle, const ws_client::message_ptr &response) {
            vorti::applets::obs_plugin::websocket_message_handler(connection_handle, response);
        });
    connection->set_close_handler([](const websocketpp::connection_hdl &connection_handle) {
        vorti::applets::obs_plugin::websocket_close_handler(connection_handle);
    });
    connection->set_fail_handler([](const websocketpp::connection_hdl &connection_handle) {
        vorti::applets::obs_plugin::websocket_fail_handler(connection_handle);
    });

    if (!m_subprotocol.empty())
    {
        connection->add_subprotocol(m_subprotocol);
    }

    m_connection_handle = connection->get_handle();

    return connection;
}

// ============================================================================
// mDNS Discovery Functions
// ============================================================================

bool vorti::applets::obs_plugin::discover_vortideck_service()
{
    if (!m_use_mdns_discovery) {
        log_to_obs("mDNS discovery disabled, using fallback connection");
        return false;
    }

    log_to_obs("Starting mDNS discovery for VortiDeck services...");
    
    // Initialize mDNS discovery if needed
    if (!m_mdns_discovery) {
        m_mdns_discovery = std::make_unique<MDNSDiscovery>();
    }

    // Set discovery in progress flag
    m_discovery_in_progress = true;

    try {
        // Perform synchronous discovery with 5-second timeout
        auto services = m_mdns_discovery->discover_services(std::chrono::seconds(5), false);
        
        m_discovery_in_progress = false;

        if (services.empty()) {
            log_to_obs("No VortiDeck services found via mDNS");
            return false;
        }

        // Use the first discovered service
        const auto& service = services[0];
        m_discovered_websocket_url = service.websocket_url;
        
        // Extract port from the URL for backward compatibility
        if (service.port > 0) {
            m_current_port = service.port;
        }

        log_to_obs(std::format("Discovered VortiDeck service: {}", m_discovered_websocket_url).c_str());
        return true;

    } catch (const std::exception& e) {
        log_to_obs(std::format("mDNS discovery error: {}", e.what()).c_str());
        m_discovery_in_progress = false;
        return false;
    }
}

bool vorti::applets::obs_plugin::discover_vortideck_service_async()
{
    if (!m_use_mdns_discovery) {
        return false;
    }

    log_to_obs("Starting asynchronous mDNS discovery...");

    // Initialize mDNS discovery if needed
    if (!m_mdns_discovery) {
        m_mdns_discovery = std::make_unique<MDNSDiscovery>();
    }

    // Set discovery in progress flag
    m_discovery_in_progress = true;

    try {
        // Check if discovery is already in progress to avoid conflicts
        if (!m_mdns_discovery->is_discovering()) {
            // Start asynchronous discovery
            m_mdns_discovery->discover_services_async(
                [](const ServiceInfo& service) {
                    // Call the static function directly
                    vorti::applets::obs_plugin::on_service_discovered(service);
                },
                std::chrono::seconds(5),
                false
            );
            return true;
        } else {
            log_to_obs("Discovery already in progress, skipping new request");
            return false;
        }

    } catch (const std::exception& e) {
        log_to_obs(std::format("Async mDNS discovery error: {}", e.what()).c_str());
        m_discovery_in_progress = false;
        return false;
    }
}

void vorti::applets::obs_plugin::on_service_discovered(const ServiceInfo& service)
{
    log_to_obs(std::format("Discovered VortiDeck service: {} at {}", service.name, service.websocket_url));
    
    // Add to discovered services list (thread-safe)
    {
        std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
        
        // Check if this service is already in our list by IP and port
        auto it = std::find_if(m_discovered_services.begin(), m_discovered_services.end(),
            [&service](const ServiceInfo& s) {
                return s.ip_address == service.ip_address && s.port == service.port;
            });
        
        if (it != m_discovered_services.end()) {
            // Update existing service
            *it = service;
            log_to_obs(std::format("Updated existing service: {}", service.name));
        } else {
            // Add new service
            m_discovered_services.push_back(service);
            log_to_obs(std::format("Added new service: {} (total: {})", service.name, m_discovered_services.size()));
            
            // Check if we have a pending dialog request from early startup
            {
                std::lock_guard<std::mutex> dialog_lock(m_dialog_mutex);
                if (m_pending_dialog_request) {
                    log_to_obs("Pending dialog request found - showing dialog now that services are available");
                    m_pending_dialog_request = false;
                    
                    // Show the dialog asynchronously to avoid blocking discovery
                    QMetaObject::invokeMethod(qApp, []() {
                        if (g_obs_plugin_instance) {
                            g_obs_plugin_instance->show_connection_settings_dialog();
                        }
                    }, Qt::QueuedConnection);
                }
            }
            
            // Update dialog ONLY if it has services (not empty dialog)
            // This prevents crash when dialog opened before any services discovered
            if (m_discovered_services.size() >= 1) {
                // Check if dialog exists and is visible
                if (qApp && m_persistent_dialog) {
                    ServiceSelectionDialog* dialog_ptr = static_cast<ServiceSelectionDialog*>(m_persistent_dialog);
                    std::vector<ServiceInfo> services_copy = m_discovered_services;
                    QMetaObject::invokeMethod(qApp, [dialog_ptr, services_copy]() {
                        // Only update if dialog is visible (not hidden/closing)
                        if (dialog_ptr && dialog_ptr->isVisible()) {
                            try {
                                dialog_ptr->updateServiceList(services_copy);
                                log_to_obs("Updated dialog with new service list");
                            } catch (...) {
                                log_to_obs("Exception updating dialog service list - ignored");
                            }
                        }
                    }, Qt::QueuedConnection);
                }
            }
            
            // Reset dialog flag if we now have multiple services
            if (m_discovered_services.size() > 1) {
                m_show_selection_dialog = false;
            }
        }
        
        m_last_discovery_time = std::chrono::steady_clock::now();
    }
    
    // Store the discovered service information for immediate use
    m_discovered_websocket_url = service.websocket_url;
    
    // Extract port for backward compatibility
    if (service.port > 0) {
        m_current_port = service.port;
    }

    // Save state for reconnection
    save_discovered_service_state(service);
    
    // Mark that we found a service
    m_service_found = true;
    m_discovery_in_progress = false;
    
    // Notify waiting threads that discovery is complete
    m_compressor_ready_cv.notify_all();
}

std::string vorti::applets::obs_plugin::get_connection_url()
{
    return get_best_available_service_url();
}


void vorti::applets::obs_plugin::websocket_open_handler(const websocketpp::connection_hdl &connection_handle)
{
    if (m_shutting_down) {
        return;
    }

    log_to_obs("WebSocket connection opened, starting initialization...");
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = true;
    }
    m_compressor_ready_cv.notify_one();

    // Send registration immediately after connection is established
    if (!register_integration()) {
        log_to_obs("Failed to send registration message");
        disconnect();
        return;
    }
    log_to_obs("Registration message sent");
    
    // Auto-update all existing overlay URLs to use the connected server
    update_all_overlay_urls_to_connected_server();
    
    // Auto-update banner URL to use the connected server
    update_all_banner_urls_to_connected_server();
}


void vorti::applets::obs_plugin::websocket_message_handler(const websocketpp::connection_hdl &connection_handle,
                                                          const ws_client::message_ptr &response)
{
    // CRITICAL: Check shutdown flag immediately to prevent processing messages after module unload
    if (m_shutting_down) {
        return;
    }

    /* Received a message, handle it */
    std::string payload = response->get_payload();
    log_to_obs("Received payload: " + payload);

    if (payload.empty())
    {
        log_to_obs("Payload is empty, returning.");
        return;
    }

    try
    {
        log_to_obs("Parsing JSON payload...");
        nlohmann::json message = nlohmann::json::parse(payload);

        // Safely check for error results
        if (message.contains("result") && !message["result"].is_null() && 
            message["result"].contains("code") && !message["result"]["code"].is_null()) {
            std::string result_code = message["result"]["code"].get<std::string>();
            if (result_code != "SUCCESS") {
                log_to_obs("Error received in result: " + result_code);
                return;
            }
        }

        // Handle registration response first  
        if (message.contains("verb") && !message["verb"].is_null() && 
            message.contains("path") && !message["path"].is_null() &&
            (message["verb"].get<std::string>() == "SET") &&
            (message["path"].get<std::string>() == "/api/v1/integration/register"))
        {
            log_to_obs("Registration response received, initializing actions...");
            if (!initialize_actions()) {
                log_to_obs("Failed to initialize actions after registration");
                return;
            }
            return;
        }

        // Handle activation response
        if (message.contains("verb") && !message["verb"].is_null() && 
            message.contains("path") && !message["path"].is_null() &&
            (message["verb"].get<std::string>() == "SET") &&
            (message["path"].get<std::string>() == "/api/v1/integration/activate"))
        {
            log_to_obs("Activation response received");
            auto instance_info_payload = message["payload"];

            if (!instance_info_payload.is_null())
            {
                std::string new_integration_guid = instance_info_payload["integrationGuid"].get<std::string>();
                std::string new_integration_instance = instance_info_payload["instanceGuid"].get<std::string>();

                if (m_integration_guid.empty() && !new_integration_guid.empty() && m_integration_instance.empty()
                    && !new_integration_instance.empty())
                {
                    m_integration_guid = new_integration_guid;
                    m_integration_instance = new_integration_instance;
                    log_to_obs("Integration activated with GUID: " + new_integration_guid);

                    // Register actions after successful activation
                    log_to_obs("Registering regular actions...");
                    register_regular_actions();
                    
                    if (helper_populate_collections()) {
                        log_to_obs("Registering parameterized actions...");
                        register_parameter_actions();
                    }

                    if (!register_actions_broadcast()) {
                        log_to_obs("Failed to register action broadcast");
                        return;
                    }

                    // Start the status update loop after successful registration
                    log_to_obs("Starting status update loop...");
                    start_loop();
                    
                    // Send initial canvas size update
                    log_to_obs("Sending initial canvas size update...");
                    send_canvas_size_update();
                }

                m_initialization_cv.notify_all();
                
                // Update dialog to show connection is established (with crash protection)
                if (qApp && !m_selected_service_url.empty() && m_persistent_dialog) {
                    ServiceSelectionDialog* dialog_ptr = static_cast<ServiceSelectionDialog*>(m_persistent_dialog);
                    std::string connected_url = m_selected_service_url;
                    std::vector<ServiceInfo> services_copy;
                    {
                        std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
                        services_copy = m_discovered_services;
                    }
                    
                    QMetaObject::invokeMethod(qApp, [dialog_ptr, connected_url, services_copy]() {
                        // Only update if dialog is visible (not hidden/closing)
                        if (dialog_ptr && dialog_ptr->isVisible()) {
                            try {
                                // Find the connected service and mark it as connected
                                for (size_t i = 0; i < services_copy.size(); ++i) {
                                    if (services_copy[i].websocket_url == connected_url) {
                                        dialog_ptr->markServiceAsConnected(static_cast<int>(i));
                                        log_to_obs(std::format("Dialog updated - service {} now connected", services_copy[i].name));
                                        break;
                                    }
                                }
                            } catch (...) {
                                log_to_obs("Exception updating dialog connection status - ignored");
                            }
                        }
                    }, Qt::QueuedConnection);
                }
            }
            return;
        }

        // Handle canvas size request (direct message type)
        if (message.contains("type") && !message["type"].is_null() &&
            message["type"].get<std::string>() == "GET_OBS_CANVAS_SIZE") {
            log_to_obs("Received canvas size request");
            handle_canvas_size_request(message);
            return;
        }

        // Handle server_info messages (these don't have verb/path fields)
        if (message.contains("action") && !message["action"].is_null() && 
            message["action"].get<std::string>() == "server_info") {
            log_to_obs("Received server_info message - ignoring");
            return;
        }

        // Safely check for verb and path fields before accessing them
        if (!message.contains("verb") || !message.contains("path") || 
            message["verb"].is_null() || message["path"].is_null()) {
            log_to_obs("DEBUG: Message missing verb/path fields - ignoring");
            return;
        }

        // Rest of the message handling...
        std::string verb = message["verb"].get<std::string>();
        std::string path = message["path"].get<std::string>();
        log_to_obs(std::format("DEBUG: Processing message with verb: {}, path: {}", verb, path));

        if ((verb == "BROADCAST") && (path == "/api/v1/integration/sdk/action/invoke"))
        {
            log_to_obs("DEBUG: Received BROADCAST /api/v1/integration/sdk/action/invoke");

            auto invoked_action = message["payload"];

            if (!invoked_action.is_null())
            {
                log_to_obs("DEBUG: Payload is not null, checking integration GUID");
                log_to_obs("DEBUG: Expected GUID: '" + m_integration_guid + "'");
                
                if (invoked_action.contains("integrationGuid")) {
                    std::string received_guid = invoked_action["integrationGuid"].get<std::string>();
                    log_to_obs("DEBUG: Received GUID: '" + received_guid + "'");
                    
                    if (received_guid == m_integration_guid) {
                        log_to_obs("DEBUG: Integration GUID matches!");

                        action_invoke_parameters parameters;
                        std::string action_id;
                        bool extraction_succeeded = false;
                        
                        try {
                            action_id = invoked_action["actionId"];
                            log_to_obs("DEBUG: Received action_id = '" + action_id + "'");
                            
                            // Manually extract parameters with type conversion
                            auto json_params = invoked_action["parameters"];
                            for (auto it = json_params.begin(); it != json_params.end(); ++it) {
                                std::string key = it.key();
                                const auto& value = it.value();
                                
                                if (value.is_string()) {
                                    parameters[key] = value.get<std::string>();
                                } else if (value.is_number()) {
                                    parameters[key] = std::to_string(value.get<double>());
                                } else if (value.is_boolean()) {
                                    parameters[key] = value.get<bool>() ? "true" : "false";
                                } else {
                                    parameters[key] = value.dump(); // Fallback to JSON string
                                }
                            }
                            log_to_obs(std::format("DEBUG: Parameters extracted and converted successfully ({} params)", parameters.size()));
                            extraction_succeeded = true;
                            
                        } catch (const std::exception& e) {
                            log_to_obs("ERROR: Failed to extract action parameters: " + std::string(e.what()));
                            log_to_obs("ERROR: Skipping action execution due to parameter error");
                            // Don't return - just skip action execution to keep websocket alive
                        }

                        if (extraction_succeeded) {
                            if (action_id == actions::s_stream_start)
                            {
                                action_stream_start(parameters);
                            }
                            else if (action_id == actions::s_stream_stop)
                    {
                        action_stream_stop(parameters);
                    }
                    else if (action_id == actions::s_stream_toggle)
                    {
                        action_stream_toggle(parameters);
                    }
                    else if (action_id == actions::s_recording_start)
                    {
                        action_recording_start(parameters);
                    }
                    else if (action_id == actions::s_recording_stop)
                    {
                        action_recording_stop(parameters);
                    }
                    else if (action_id == actions::s_recording_toggle)
                    {
                        action_recording_toggle(parameters);
                    }
                    else if (action_id == actions::s_buffer_start)
                    {
                        action_buffer_start(parameters);
                    }
                    else if (action_id == actions::s_buffer_stop)
                    {
                        action_buffer_stop(parameters);
                    }
                    else if (action_id == actions::s_buffer_toggle)
                    {
                        action_buffer_toggle(parameters);
                    }
                    else if (action_id == actions::s_buffer_save)
                    {
                        action_buffer_save(parameters);
                    }
                    else if (action_id == actions::s_desktop_mute)
                    {
                        action_desktop_mute(parameters);
                    }
                    else if (action_id == actions::s_desktop_unmute)
                    {
                        action_desktop_unmute(parameters);
                    }
                    else if (action_id == actions::s_desktop_mute_toggle)
                    {
                        action_desktop_mute_toggle(parameters);
                    }
                    else if (action_id == actions::s_mic_mute)
                    {
                        action_mic_mute(parameters);
                    }
                    else if (action_id == actions::s_mic_unmute)
                    {
                        action_mic_unmute(parameters);
                    }
                    else if (action_id == actions::s_mic_mute_toggle)
                    {
                        action_mic_mute_toggle(parameters);
                    }
                    else if (action_id == actions::s_collection_activate)
                    {
                        action_collection_activate(parameters);
                    }
                    else if (action_id == actions::s_scenes_activate)
                    {
                        action_scene_activate(parameters);
                    }
                    else if (action_id == actions::s_source_activate)
                    {
                        action_source_activate(parameters);
                    }
                    else if (action_id == actions::s_source_deactivate)
                    {
                        action_source_deactivate(parameters);
                    }
                    else if (action_id == actions::s_source_toggle)
                    {
                        action_source_toggle(parameters);
                    }
                    else if (action_id == actions::s_mixer_mute)
                    {
                        action_mixer_mute(parameters);
                    }
                    else if (action_id == actions::s_mixer_unmute)
                    {
                        action_mixer_unmute(parameters);
                    }
                    else if (action_id == actions::s_mixer_mute_toggle)
                    {
                        action_mixer_mute_toggle(parameters);
                    }
                        // Banner action handlers
    else if constexpr (BANNER_MANAGER_ENABLED) {
        if (action_id == actions::s_banner_show)
        {
            action_banner_show(parameters);
        }
        else if (action_id == actions::s_banner_hide)
        {
            action_banner_hide(parameters);
        }
        else if (action_id == actions::s_banner_toggle)
        {
            action_banner_toggle(parameters);
        }
        // Overlay action handlers (no restrictions)
        else if (action_id == actions::s_overlay_set_data)
        {
            action_overlay_set_data(parameters);
        }
        else if (action_id == actions::s_overlay_create)
        {
            action_overlay_create(parameters);
        }
        else if (action_id == actions::s_overlay_update)
        {
            action_overlay_update(parameters);
        }
        else if (action_id == actions::s_overlay_remove)
        {
            action_overlay_remove(parameters);
        }
        // Complex banner actions removed - handled by external application
    }
                            else
                            {
                                log_to_obs("DEBUG: Unhandled action_id: '" + action_id + "'");
                            }
                        } // End of if (extraction_succeeded)
                    } else {
                        log_to_obs("DEBUG: Integration GUID mismatch!");
                    }
                } else {
                    log_to_obs("DEBUG: No integrationGuid field in payload");
                }
            } else {
                log_to_obs("DEBUG: Payload is null");
            }
        }
    }
    catch (std::exception& e)
    {
        log_to_obs("ERROR: Exception in message handling: " + std::string(e.what()));
    }
}


void vorti::applets::obs_plugin::websocket_fail_handler(const websocketpp::connection_hdl &connection_handle)
{
    /* Something failed! ABORT THE MISSION! ALT+F4 */
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
    }

    stop_loop();
}


bool vorti::applets::obs_plugin::register_integration()
{
    if (!is_connected())
    {
        log_to_obs("Cannot register integration - not connected");
        return false;
    }

    log_to_obs("Sending registration message...");

    // clang-format off
    nlohmann::json register_app =
    {
        { "path", "/api/v1/integration/register" },
        { "verb", "SET" },
        { "payload",
            {
                { "integrationIdentifier", s_integration_identifier },
                { "name", s_integration_name },
                { "author", s_integration_author },
                { "description", s_integration_description },
                { "icon", s_integration_icon_1 + s_integration_icon_2 },
                { "manualRegistration", true }
            }
        }
    };
    // clang-format on

    return send_message(register_app);
}


bool vorti::applets::obs_plugin::initialize_actions()
{
    if (!is_connected())
    {
        return false;
    }

    // clang-format off
    nlohmann::json initalize_actions =
    {
        { "path", "/api/v1/integration/activate" },
        { "verb", "SET" },
        { "payload",
            {
                { "integrationIdentifier", s_integration_identifier },
                { "sdkType", "ACTION" }
            }
        }
    };
    // clang-format on

    send_message(initalize_actions);

    // Don't wait here - let the main connect() function handle the timeout
    // The activation response will be handled by websocket_message_handler
    // which will notify m_initialization_cv when ready
    return true;
}


void vorti::applets::obs_plugin::uninitialize_actions()
{
    if (!is_connected())
    {
        return;
    }

    // clang-format off
    nlohmann::json uninitalize_actions =
    {
        { "path", "/api/v1/integration/deactivate" },
        { "verb", "SET" },
        { "payload",
            {
                { "integrationIdentifier", s_integration_identifier },
                { "instanceGuid", m_integration_instance },
                { "sdkType", "ACTION" }
            }
        }
    };
    // clang-format on

    send_message(uninitalize_actions);
}


nlohmann::json vorti::applets::obs_plugin::register_action(const std::string &action_id,
                                                          const std::string &action_name,
                                                          const action_parameters &parameters)
{
    // clang-format off
    nlohmann::json action =
    {
        { "actionId", action_id },
        { "name", action_name },
        { "parameters", parameters }
    };
    // clang-format on

    return action;
}


bool vorti::applets::obs_plugin::register_actions_broadcast()
{
    if (!is_connected())
    {
        return false;
    }

    // clang-format off
    nlohmann::json message =
    {
        { "path", "/api/v1/integration/sdk/action/invoke" },
        { "verb", "SUBSCRIBE" }
    };
    // clang-format on

    return send_message(message);
}


void vorti::applets::obs_plugin::register_regular_actions()
{
    if (!is_connected())
    {
        return;
    }

    std::vector<nlohmann::json> actions;
    actions.push_back(register_action(actions::s_stream_start, "APPLET_OBS_START_STREAM"));
    actions.push_back(register_action(actions::s_stream_stop, "APPLET_OBS_STOP_STREAM"));
    actions.push_back(register_action(actions::s_stream_toggle, "APPLET_OBS_TOGGLE_STREAM"));
    actions.push_back(register_action(actions::s_recording_start, "APPLET_OBS_START_RECORDING"));
    actions.push_back(register_action(actions::s_recording_stop, "APPLET_OBS_STOP_RECORDING"));
    actions.push_back(register_action(actions::s_recording_toggle, "APPLET_OBS_TOGGLE_RECORDING"));
    actions.push_back(register_action(actions::s_buffer_start, "APPLET_OBS_START_BUFFER"));
    actions.push_back(register_action(actions::s_buffer_stop, "APPLET_OBS_STOP_BUFFER"));
    actions.push_back(register_action(actions::s_buffer_toggle, "APPLET_OBS_TOGGLE_BUFFER"));
    actions.push_back(register_action(actions::s_buffer_save, "APPLET_OBS_SAVE_BUFFER"));
    actions.push_back(register_action(actions::s_desktop_mute, "APPLET_OBS_DESKTOP_MUTE"));
    actions.push_back(register_action(actions::s_desktop_unmute, "APPLET_OBS_DESKTOP_UNMUTE"));
    actions.push_back(register_action(actions::s_desktop_mute_toggle, "APPLET_OBS_DESKTOP_MUTE_TOGGLE"));
    actions.push_back(register_action(actions::s_mic_mute, "APPLET_OBS_MIC_MUTE"));
    actions.push_back(register_action(actions::s_mic_unmute, "APPLET_OBS_MIC_UNMUTE"));
    actions.push_back(register_action(actions::s_mic_mute_toggle, "APPLET_OBS_MIC_MUTE_TOGGLE"));

    // ADD THESE BANNER ACTIONS (simplified - no data setting, banners always use connected service URL)
    if constexpr (BANNER_MANAGER_ENABLED) {
        actions.push_back(register_action(actions::s_banner_show, "APPLET_OBS_BANNER_SHOW"));
        actions.push_back(register_action(actions::s_banner_hide, "APPLET_OBS_BANNER_HIDE"));
        actions.push_back(register_action(actions::s_banner_toggle, "APPLET_OBS_BANNER_TOGGLE"));
        // Removed APPLET_OBS_BANNER_SET_DATA - banners now use connected service URL + /banners like overlays
    }
    
    // Register overlay actions (always available, no restrictions)
    {
        // Main overlay set data action (like banner_set_data)
        action_parameters overlay_set_data_params;
        nlohmann::json url_param_main = {
            {"name", "url"},
            {"displayName", "URL"},
            {"description", "URL for the overlay content"}
        };
        overlay_set_data_params.push_back(url_param_main);
        actions.push_back(register_action(actions::s_overlay_set_data, "APPLET_OBS_OVERLAY_SET_DATA", overlay_set_data_params));
        
        // Overlay create action
        action_parameters overlay_create_params;
        nlohmann::json overlay_id_param = {
            {"name", "overlay_id"},
            {"displayName", "Overlay ID"},
            {"description", "Unique identifier for the overlay"}
        };
        nlohmann::json url_param_create = {
            {"name", "url"},
            {"displayName", "URL"},
            {"description", "URL for the overlay content"}
        };
        nlohmann::json name_param = {
            {"name", "name"},
            {"displayName", "Source Name"},
            {"description", "Optional custom name for the source"}
        };
        nlohmann::json scene_param = {
            {"name", "scene_name"},
            {"displayName", "Scene Name"},
            {"description", "Optional scene to add the overlay to"}
        };
        nlohmann::json width_param_overlay = {
            {"name", "width"},
            {"displayName", "Width"},
            {"description", "Optional width in pixels (default: 1920)"}
        };
        nlohmann::json height_param_overlay = {
            {"name", "height"},
            {"displayName", "Height"},
            {"description", "Optional height in pixels (default: 1080)"}
        };
        overlay_create_params.push_back(overlay_id_param);
        overlay_create_params.push_back(url_param_create);
        overlay_create_params.push_back(name_param);
        overlay_create_params.push_back(scene_param);
        overlay_create_params.push_back(width_param_overlay);
        overlay_create_params.push_back(height_param_overlay);
        actions.push_back(register_action(actions::s_overlay_create, "APPLET_OBS_OVERLAY_CREATE", overlay_create_params));
        
        // Overlay update action
        action_parameters overlay_update_params;
        nlohmann::json source_name_param = {
            {"name", "source_name"},
            {"displayName", "Source Name"},
            {"description", "Name of the overlay source to update"}
        };
        overlay_update_params.push_back(source_name_param);
        overlay_update_params.push_back(url_param_create);
        overlay_update_params.push_back(width_param_overlay);
        overlay_update_params.push_back(height_param_overlay);
        actions.push_back(register_action(actions::s_overlay_update, "APPLET_OBS_OVERLAY_UPDATE", overlay_update_params));
        
        // Overlay remove action
        action_parameters overlay_remove_params;
        overlay_remove_params.push_back(source_name_param);
        actions.push_back(register_action(actions::s_overlay_remove, "APPLET_OBS_OVERLAY_REMOVE", overlay_remove_params));
    }

    // Complex banner actions removed - simplified banner system
    // Only core banner actions remain: show, hide, toggle, set_data

    // clang-format off
    nlohmann::json register_actions =
    {
        { "path", "/api/v1/actions/register" },
        { "verb", "SET" },
        { "payload",
            {
                { "actions", actions },
                { "instance",
                    {
                        { "integrationGuid", m_integration_guid },
                        { "instanceGuid", m_integration_instance }
                    }
                }
            }
        }
    };
    // clang-format on

    std::string new_actions = register_actions.dump();

    if (new_actions != registered_regular_actions)
    {
        registered_regular_actions = new_actions;
        send_message(register_actions);
    }
}


void vorti::applets::obs_plugin::register_parameter_actions()
{
    if (m_shutting_down)
    {
        // Let's not do any enumeration on shutdown
        return;
    }

    if (!is_connected())
    {
        return;
    }

    auto collections = helper_get_available_collections();
    auto scenes = helper_get_available_scenes();
    auto sources = helper_get_available_sources();
    auto mixers = helper_get_available_mixers();

    std::vector<nlohmann::json> actions;
    actions.push_back(register_action(actions::s_collection_activate, "APPLET_OBS_COLLECTION_ACTIVATE", collections));
    actions.push_back(register_action(actions::s_scenes_activate, "APPLET_OBS_SCENE_ACTIVATE", scenes));
    actions.push_back(register_action(actions::s_source_activate, "APPLET_OBS_SOURCE_ACTIVATE", sources));
    actions.push_back(register_action(actions::s_source_deactivate, "APPLET_OBS_SOURCE_DEACTIVATE", sources));
    actions.push_back(register_action(actions::s_source_toggle, "APPLET_OBS_SOURCE_TOGGLE", sources));
    actions.push_back(register_action(actions::s_mixer_mute, "APPLET_OBS_MIXER_MUTE", mixers));
    actions.push_back(register_action(actions::s_mixer_unmute, "APPLET_OBS_MIXER_UNMUTE", mixers));
    actions.push_back(register_action(actions::s_mixer_mute_toggle, "APPLET_OBS_MIXER_MUTE_TOGGLE", mixers));

    // clang-format off
    nlohmann::json register_actions =
    {
        { "path", "/api/v1/actions/register" },
        { "verb", "SET" },
        { "payload",
            {
                { "actions", actions },
                { "instance",
                    {
                        { "integrationGuid", m_integration_guid },
                        { "instanceGuid", m_integration_instance }
                    }
                }
            }
        }
    };
    // clang-format on

    std::string new_actions = register_actions.dump();

    if (new_actions != registered_parametarized_actions)
    {
        registered_parametarized_actions = new_actions;
        send_message(register_actions);
    }
}


void vorti::applets::obs_plugin::action_stream_start(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (!obs_frontend_streaming_active())
    {
        obs_frontend_streaming_start();
    }
}


void vorti::applets::obs_plugin::action_stream_stop(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (obs_frontend_streaming_active())
    {
        obs_frontend_streaming_stop();
    }
}


void vorti::applets::obs_plugin::action_stream_toggle(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (obs_frontend_streaming_active())
    {
        obs_frontend_streaming_stop();
    }
    else
    {
        obs_frontend_streaming_start();
    }
}


void vorti::applets::obs_plugin::action_recording_start(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (!obs_frontend_recording_active())
    {
        obs_frontend_recording_start();
    }
}


void vorti::applets::obs_plugin::action_recording_stop(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (obs_frontend_recording_active())
    {
        obs_frontend_recording_stop();
    }
}


void vorti::applets::obs_plugin::action_recording_toggle(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (obs_frontend_recording_active())
    {
        obs_frontend_recording_stop();
    }
    else
    {
        obs_frontend_recording_start();
    }
}


void vorti::applets::obs_plugin::action_buffer_start(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (!obs_frontend_replay_buffer_active())
    {
        obs_frontend_replay_buffer_start();
    }
}


void vorti::applets::obs_plugin::action_buffer_stop(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (obs_frontend_replay_buffer_active())
    {
        obs_frontend_replay_buffer_stop();
    }
}


void vorti::applets::obs_plugin::action_buffer_toggle(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    if (obs_frontend_replay_buffer_active())
    {
        obs_frontend_replay_buffer_stop();
    }
    else
    {
        obs_frontend_replay_buffer_start();
    }
}


void vorti::applets::obs_plugin::action_buffer_save(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    obs_frontend_replay_buffer_save();
}


void vorti::applets::obs_plugin::action_desktop_mute(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    helper_desktop_mute(true, false);
}


void vorti::applets::obs_plugin::action_desktop_unmute(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    helper_desktop_mute(false, false);
}


void vorti::applets::obs_plugin::action_desktop_mute_toggle(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    helper_desktop_mute(true, true);
}


void vorti::applets::obs_plugin::action_mic_mute(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    helper_mic_mute(true, false);
}


void vorti::applets::obs_plugin::action_mic_unmute(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    helper_mic_mute(false, false);
}


void vorti::applets::obs_plugin::action_mic_mute_toggle(const action_invoke_parameters &parameters)
{
    // This action requires 0 parameters
    if (!parameters.empty())
    {
        return;
    }

    helper_mic_mute(true, true);
}


void vorti::applets::obs_plugin::action_collection_activate(const action_invoke_parameters &parameters)
{
    // This action requires exactly 1 parameter
    if (parameters.size() != 1)
    {
        return;
    }

    auto collection_name_find = parameters.find(actions::parameters::s_collection_name);
    if (collection_name_find == parameters.end())
    {
        return;
    }

    obs_frontend_set_current_scene_collection(collection_name_find->second.c_str());
}


void vorti::applets::obs_plugin::action_scene_activate(const action_invoke_parameters &parameters)
{
    // This action requires exactly 1 parameter
    if (parameters.size() != 1)
    {
        return;
    }

    auto scene_name_find = parameters.find(actions::parameters::s_scene_name);
    if (scene_name_find == parameters.end())
    {
        return;
    }

    std::string scene_name = scene_name_find->second;

    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    for (size_t i = 0; i < scenes.sources.num; i++)
    {
        obs_source_t *source = scenes.sources.array[i];

        if (!source)
        {
            continue;
        }

        std::string str_name(obs_source_get_name(source));

        if (str_name == scene_name)
        {
            obs_frontend_set_current_scene(source);
            break;
        }
    }
    obs_frontend_source_list_free(&scenes);
}


void vorti::applets::obs_plugin::action_source_activate(const action_invoke_parameters &parameters)
{
    // This action requires exactly 2 parameters
    if (parameters.size() != 2)
    {
        return;
    }

    auto scene_name_find = parameters.find(actions::parameters::s_scene_name);
    if (scene_name_find == parameters.end())
    {
        return;
    }

    auto source_name_find = parameters.find(actions::parameters::s_source_name);
    if (source_name_find == parameters.end())
    {
        return;
    }

    std::string scene_name = scene_name_find->second;
    std::string source_name = source_name_find->second;

    helper_source_activate(scene_name, source_name, true, false);
}


void vorti::applets::obs_plugin::action_source_deactivate(const action_invoke_parameters &parameters)
{
    // This action requires exactly 2 parameters
    if (parameters.size() != 2)
    {
        return;
    }

    auto scene_name_find = parameters.find(actions::parameters::s_scene_name);
    if (scene_name_find == parameters.end())
    {
        return;
    }

    auto source_name_find = parameters.find(actions::parameters::s_source_name);
    if (source_name_find == parameters.end())
    {
        return;
    }

    std::string scene_name = scene_name_find->second;
    std::string source_name = source_name_find->second;

    helper_source_activate(scene_name, source_name, false, false);
}


void vorti::applets::obs_plugin::action_source_toggle(const action_invoke_parameters &parameters)
{
    // This action requires exactly 2 parameters
    if (parameters.size() != 2)
    {
        return;
    }

    auto scene_name_find = parameters.find(actions::parameters::s_scene_name);
    if (scene_name_find == parameters.end())
    {
        return;
    }

    auto source_name_find = parameters.find(actions::parameters::s_source_name);
    if (source_name_find == parameters.end())
    {
        return;
    }

    std::string scene_name = scene_name_find->second;
    std::string source_name = source_name_find->second;

    helper_source_activate(scene_name, source_name, true, true);
}


void vorti::applets::obs_plugin::action_mixer_mute(const action_invoke_parameters &parameters)
{
    // This action requires exactly 2 parameters
    if (parameters.size() != 2)
    {
        return;
    }

    auto scene_name_find = parameters.find(actions::parameters::s_scene_name);
    if (scene_name_find == parameters.end())
    {
        return;
    }

    auto mixer_name_find = parameters.find(actions::parameters::s_mixer_name);
    if (mixer_name_find == parameters.end())
    {
        return;
    }

    std::string scene_name = scene_name_find->second;
    std::string mixer_name = mixer_name_find->second;

    helper_mixer_mute(scene_name, mixer_name, true, false);
}


void vorti::applets::obs_plugin::action_mixer_unmute(const action_invoke_parameters &parameters)
{
    // This action requires exactly 2 parameters
    if (parameters.size() != 2)
    {
        return;
    }

    auto scene_name_find = parameters.find(actions::parameters::s_scene_name);
    if (scene_name_find == parameters.end())
    {
        return;
    }

    auto mixer_name_find = parameters.find(actions::parameters::s_mixer_name);
    if (mixer_name_find == parameters.end())
    {
        return;
    }

    std::string scene_name = scene_name_find->second;
    std::string mixer_name = mixer_name_find->second;

    helper_mixer_mute(scene_name, mixer_name, false, false);
}


void vorti::applets::obs_plugin::action_mixer_mute_toggle(const action_invoke_parameters &parameters)
{
    // This action requires exactly 2 parameters
    if (parameters.size() != 2)
    {
        return;
    }

    auto scene_name_find = parameters.find(actions::parameters::s_scene_name);
    if (scene_name_find == parameters.end())
    {
        return;
    }

    auto mixer_name_find = parameters.find(actions::parameters::s_mixer_name);
    if (mixer_name_find == parameters.end())
    {
        return;
    }

    std::string scene_name = scene_name_find->second;
    std::string mixer_name = mixer_name_find->second;

    helper_mixer_mute(scene_name, mixer_name, true, true);
}


void vorti::applets::obs_plugin::helper_desktop_mute(bool new_state, bool is_toggle)
{
    // Iterate over channels attempting to set mute
    for (int channel = 1; channel <= 2; channel++)
    {
        obs_source_t *sceneUsed = obs_get_output_source(channel);

        if (!sceneUsed)
        {
            continue;
        }

        if (is_toggle)
        {
            if (obs_source_muted(sceneUsed))
            {
                obs_source_set_muted(sceneUsed, false);
            }
            else
            {
                obs_source_set_muted(sceneUsed, true);
            }
        }
        else
        {
            obs_source_set_muted(sceneUsed, new_state);
        }

        obs_source_release(sceneUsed);
    }
}


void vorti::applets::obs_plugin::helper_mic_mute(bool new_state, bool is_toggle)
{
    // Iterate over channels attempting to set mute
    for (int channel = 3; channel <= 5; channel++)
    {
        obs_source_t *sceneUsed = obs_get_output_source(channel);

        if (!sceneUsed)
        {
            continue;
        }

        if (is_toggle)
        {
            if (obs_source_muted(sceneUsed))
            {
                obs_source_set_muted(sceneUsed, false);
            }
            else
            {
                obs_source_set_muted(sceneUsed, true);
            }
        }
        else
        {
            obs_source_set_muted(sceneUsed, new_state);
        }

        obs_source_release(sceneUsed);
    }
}


void vorti::applets::obs_plugin::helper_source_activate(const std::string &scene_name,
                                                       const std::string &source_name,
                                                       bool new_state,
                                                       bool is_toggle)
{
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    for (size_t i = 0; i < scenes.sources.num; i++)
    {
        obs_source_t *source = scenes.sources.array[i];

        if (!source)
        {
            continue;
        }

        std::string name(obs_source_get_name(source));

        if (name != scene_name)
        {
            continue;
        }

        obs_scene_t *scene = obs_scene_from_source(source);

        if (!scene)
        {
            continue;
        }

        auto *state = new new_state_info;
        state->name = source_name;
        state->new_state = new_state;
        state->is_toggle = is_toggle;

        auto sourceEnumProc = [](obs_scene_t *scene, obs_sceneitem_t *currentItem, void *privateData) -> bool {
            auto *parameters = (new_state_info *)privateData;
            obs_source_t *source = obs_sceneitem_get_source(currentItem);
            uint32_t source_type = obs_source_get_output_flags(source);
            std::string source_name(obs_source_get_name(source));

            if (source_name == parameters->name)
            {
                if (parameters->is_toggle)
                {
                    if (obs_sceneitem_visible(currentItem))
                    {
                        obs_sceneitem_set_visible(currentItem, false);
                    }
                    else
                    {
                        obs_sceneitem_set_visible(currentItem, true);
                    }
                }
                else
                {
                    obs_sceneitem_set_visible(currentItem, parameters->new_state);
                }
            }

            return true;
        };

        obs_scene_enum_items(scene, sourceEnumProc, state);
        delete state;

        break;
    }
    obs_frontend_source_list_free(&scenes);
}


void vorti::applets::obs_plugin::helper_mixer_mute(const std::string &scene_name,
                                                  const std::string &mixer_name,
                                                  bool new_state,
                                                  bool is_toggle)
{
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    for (size_t i = 0; i < scenes.sources.num; i++)
    {
        obs_source_t *source = scenes.sources.array[i];

        if (!source)
        {
            continue;
        }

        std::string name(obs_source_get_name(source));

        if (name != scene_name)
        {
            continue;
        }

        obs_scene_t *scene = obs_scene_from_source(source);

        if (!scene)
        {
            continue;
        }

        auto *state = new new_state_info;
        state->name = mixer_name;
        state->new_state = new_state;
        state->is_toggle = is_toggle;

        auto sourceEnumProc = [](obs_scene_t *scene, obs_sceneitem_t *currentItem, void *privateData) -> bool {
            auto *parameters = (new_state_info *)privateData;
            obs_source_t *source = obs_sceneitem_get_source(currentItem);
            uint32_t source_type = obs_source_get_output_flags(source);
            std::string source_name(obs_source_get_name(source));

            if (source_name == parameters->name)
            {
                if (parameters->is_toggle)
                {
                    if (obs_source_muted(source))
                    {
                        obs_source_set_muted(source, false);
                    }
                    else
                    {
                        obs_source_set_muted(source, true);
                    }
                }
                else
                {
                    obs_source_set_muted(source, parameters->new_state);
                }
            }

            return true;
        };

        obs_scene_enum_items(scene, sourceEnumProc, state);
        delete state;

        break;
    }
    obs_frontend_source_list_free(&scenes);
}


action_parameters vorti::applets::obs_plugin::helper_get_available_collections()
{
    action_parameters available_scenes;

    std::vector<nlohmann::json> list_selection;
    for (const auto &collection : m_obs_collections)
    {
        // clang-format off
        nlohmann::json selection =
        {
            { "displayName", collection.first },
            { "value", collection.first },
        };
        // clang-format on
        list_selection.push_back(selection);
    }

    // clang-format off
    nlohmann::json collections_parameter =
    {
        { "parameterId", actions::parameters::s_collection_name },
        { "assignmentMessage", actions::messages::s_collection_assignment },
        { "errorMessage", actions::messages::s_collection_error },
        { "parameterType", "LIST" },
        { "listSelection" , list_selection }
    };
    // clang-format on

    available_scenes.push_back(collections_parameter);

    return available_scenes;
}


action_parameters vorti::applets::obs_plugin::helper_get_available_scenes()
{
    action_parameters available_scenes;

    std::string current_collection_name;

    {
        char *current_collection = obs_frontend_get_current_scene_collection();
        current_collection_name = std::string(current_collection);
        bfree(current_collection);
    }

    if (current_collection_name.empty())
    {
        return available_scenes;
    }

    auto current_collection = m_obs_collections[current_collection_name];

    std::vector<nlohmann::json> list_selection;
    for (const auto &scene : current_collection)
    {
        // clang-format off
        nlohmann::json selection =
        {
            { "displayName", scene.first },
            { "value", scene.first },
        };
        // clang-format on
        list_selection.push_back(selection);
    }

    // clang-format off
    nlohmann::json scene_parameter =
    {
        { "parameterId", actions::parameters::s_scene_name },
        { "assignmentMessage", actions::messages::s_scene_assignment },
        { "errorMessage", actions::messages::s_scene_error },
        { "parameterType", "LIST" },
        { "listSelection" , list_selection }
    };
    // clang-format on

    available_scenes.push_back(scene_parameter);

    return available_scenes;
}


action_parameters vorti::applets::obs_plugin::helper_get_available_sources()
{
    action_parameters available_scenes;

    std::string current_collection_name;

    {
        char *current_collection = obs_frontend_get_current_scene_collection();
        current_collection_name = std::string(current_collection);
        bfree(current_collection);
    }

    if (current_collection_name.empty())
    {
        return available_scenes;
    }

    auto current_collection = m_obs_collections[current_collection_name];

    std::vector<nlohmann::json> list_selection;
    for (const auto &scene : current_collection)
    {
        std::vector<nlohmann::json> list_selection_source;
        for (const auto &source : scene.second.sources)
        {
            // clang-format off
            nlohmann::json selection =
            {
                { "displayName", source },
                { "value", source },
            };
            // clang-format on
            list_selection_source.push_back(selection);
        }

        // clang-format off
        nlohmann::json source_parameter =
        {
            { "parameterId", actions::parameters::s_source_name },
            { "assignmentMessage", actions::messages::s_source_assignment },
            { "errorMessage", actions::messages::s_source_error },
            { "parameterType", "LIST" },
            { "listSelection" , list_selection_source }
        };

        nlohmann::json selection =
        {
            { "displayName", scene.first },
            { "value", scene.first },
            { "parameters", { source_parameter } }
        };
        // clang-format on
        list_selection.push_back(selection);
    }

    // clang-format off
    nlohmann::json scene_parameter =
    {
        { "parameterId", actions::parameters::s_scene_name },
        { "assignmentMessage", actions::messages::s_scene_assignment },
        { "errorMessage", actions::messages::s_scene_error },
        { "parameterType", "LIST" },
        { "listSelection" , list_selection }
    };
    // clang-format on

    available_scenes.push_back(scene_parameter);

    return available_scenes;
}


action_parameters vorti::applets::obs_plugin::helper_get_available_mixers()
{
    action_parameters available_scenes;

    std::string current_collection_name;

    {
        char *current_collection = obs_frontend_get_current_scene_collection();
        current_collection_name = std::string(current_collection);
        bfree(current_collection);
    }

    if (current_collection_name.empty())
    {
        return available_scenes;
    }

    auto current_collection = m_obs_collections[current_collection_name];

    std::vector<nlohmann::json> list_selection;
    for (const auto &scene : current_collection)
    {
        std::vector<nlohmann::json> list_selection_mixer;
        for (const auto &source : scene.second.mixers)
        {
            // clang-format off
            nlohmann::json selection =
            {
                { "displayName", source },
                { "value", source },
            };
            // clang-format on
            list_selection_mixer.push_back(selection);
        }

        // clang-format off
        nlohmann::json mixer_parameter =
        {
            { "parameterId", actions::parameters::s_mixer_name },
            { "assignmentMessage", actions::messages::s_mixer_assignment },
            { "errorMessage", actions::messages::s_mixer_error },
            { "parameterType", "LIST" },
            { "listSelection" , list_selection_mixer }
        };

        nlohmann::json selection =
        {
            { "displayName", scene.first },
            { "value", scene.first },
            { "parameters", { mixer_parameter } }
        };
        // clang-format on
        list_selection.push_back(selection);
    }

    // clang-format off
    nlohmann::json scene_parameter =
    {
        { "parameterId", actions::parameters::s_scene_name },
        { "assignmentMessage", actions::messages::s_scene_assignment },
        { "errorMessage", actions::messages::s_scene_error },
        { "parameterType", "LIST" },
        { "listSelection" , list_selection }
    };
    // clang-format on

    available_scenes.push_back(scene_parameter);

    return available_scenes;
}


bool vorti::applets::obs_plugin::helper_populate_collections()
{
    if (m_shutting_down)
    {
        // Let's not do any enumeration on shutdown
        return false;
    }

    if (m_collection_locked)
    {
        // Cannot access scene collection at this time
        return false;
    }

    std::string current_collection_name;

    {
        char *current_collection = obs_frontend_get_current_scene_collection();
        current_collection_name = std::string(current_collection);
        bfree(current_collection);
    }

    if (current_collection_name.empty())
    {
        // No current collection
        return false;
    }

    {
        char **scene_collections = obs_frontend_get_scene_collections();
        auto collection_names = scene_collections;

        // Initialize all collection names
        for (char *collection_name = *collection_names; collection_name; collection_name = *++collection_names)
        {
            m_obs_collections[std::string(collection_name)];
        }

        bfree(scene_collections);
    }

    auto &current_collection = m_obs_collections[current_collection_name];

    current_collection.clear();

    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    for (size_t i = 0; i < scenes.sources.num; i++)
    {
        obs_source_t *source = scenes.sources.array[i];

        if (!source)
        {
            continue;
        }

        const std::string name_str(obs_source_get_name(source));
        obs_scene_t *scene = obs_scene_from_source(source);

        if (!scene)
        {
            continue;
        }

        auto *obs_scenes = new scene_info;
        auto &current_collection_scene = current_collection[name_str];

        current_collection_scene.sources.clear();

        auto sourceEnumProc = [](obs_scene_t *scene, obs_sceneitem_t *currentItem, void *privateData) -> bool {
            auto *parameters = (scene_info *)privateData;

            obs_source_t *source = obs_sceneitem_get_source(currentItem);
            uint32_t source_type = obs_source_get_output_flags(source);

            if (((source_type & OBS_SOURCE_VIDEO) == OBS_SOURCE_VIDEO)
                || ((source_type & OBS_SOURCE_ASYNC) == OBS_SOURCE_ASYNC))
            {
                parameters->sources.push_back(std::string(obs_source_get_name(source)));
            }
            else if ((source_type & OBS_SOURCE_AUDIO) == OBS_SOURCE_AUDIO)
            {
                parameters->mixers.push_back(std::string(obs_source_get_name(source)));
            }

            return true;
        };
        obs_scene_enum_items(scene, sourceEnumProc, obs_scenes);

        for (const auto &scene_element : obs_scenes->sources)
        {
            current_collection_scene.sources.push_back(scene_element);
        }

        for (const auto &scene_element : obs_scenes->mixers)
        {
            current_collection_scene.mixers.push_back(scene_element);
        }

        delete obs_scenes;
    }
    obs_frontend_source_list_free(&scenes);

    return true;
}


bool vorti::applets::obs_plugin::send_message(nlohmann::json message)
{
    if (!is_connected())
    {
        return false;
    }

    std::lock_guard<std::mutex> ql(m_lock);

    message["msgId"] = std::to_string(m_current_message_id);
    std::string string_message = message.dump();

    m_current_message_id++;

    websocketpp::lib::error_code error_code;
    m_websocket.send(m_connection_handle, string_message, m_subprotocol_opcode, error_code);

    return (0 == error_code.value());
}


void vorti::applets::obs_plugin::start_loop()
{
    if (!m_loop_thread.joinable())
    {
        m_loop_thread_running = true;
        m_loop_thread = std::jthread(&loop_function);
    }
}


void vorti::applets::obs_plugin::stop_loop()
{
    bool is_joining = false;
    {
        if (m_loop_thread.joinable())
        {
            std::lock_guard<std::mutex> wlock(m_thread_lock);
            m_loop_thread_running = false;
            is_joining = true;
        }
    }

    // Stop the jthread
    if (is_joining)
    {
        m_loop_thread.join();
    }
}


void vorti::applets::obs_plugin::loop_function()
{
    os_cpu_usage_info_t *cpu_usage = os_cpu_usage_info_start();

    while (true)
    {
        // CRITICAL: Check shutdown flag first to exit immediately if shutting down
        if (m_shutting_down) {
            break;
        }

        bool is_loop_running = false;
        long update_interval = 0;

        {
            std::lock_guard<std::mutex> wlock(m_thread_lock);
            is_loop_running = m_loop_thread_running;
            update_interval = m_update_interval;
        }

        if (is_loop_running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(update_interval));
        }
        else
        {
            break;
        }

        std::lock_guard<std::mutex> wlock(m_thread_lock);

        // CRITICAL: Check shutdown flag again before calling obs_frontend functions
        if (m_shutting_down) {
            break;
        }

        // clang-format off
        // Gather obs data here...
        nlohmann::json new_status =
        {
            { "path", "/api/v1/integration/obs/status" },
            { "verb", "SET" }
        };
        // clang-format on

        auto status_payload = new_status["payload"];

        status_payload["inStudioMode"] = m_studio_mode;

        obs_output_t *obs_output = nullptr;

        if (obs_frontend_streaming_active())
        {
            obs_output = obs_frontend_get_streaming_output();
            status_payload["currentState"] = "STREAMING";
        }
        else if (obs_frontend_recording_active())
        {
            obs_output = obs_frontend_get_recording_output();
            status_payload["currentState"] = "RECORDING";
        }
        else
        {
            status_payload["currentState"] = "IDLE";
        }

        double_t bps = 0;
        double_t fps = 0;
        if (nullptr != obs_output)
        {
            // Calculate bitrate
            auto streamed_bytes = static_cast<int32_t>(obs_output_get_total_bytes(obs_output));
            int32_t bytes_per_second = streamed_bytes - m_total_streamed_bytes;
            bps = (static_cast<double_t>(bytes_per_second) / 1000)
                  * 8;  // Bytes/s converted to KiloBytes/s then converted to Kilobits/s

            m_total_streamed_bytes = streamed_bytes;

            // Calculate framerate
            int32_t streamed_frames = obs_output_get_total_frames(obs_output);
            fps = static_cast<double_t>(streamed_frames) - static_cast<double_t>(m_total_streamed_frames);

            m_total_streamed_frames = streamed_frames;
        }
        status_payload["bitrate"] = bps;
        status_payload["framerate"] = fps;

        int32_t duration_hours = 0;
        int32_t duration_minutes = 0;
        int32_t duration_seconds = 0;

        if (status_payload["currentState"] != "IDLE")
        {
            auto duration = std::chrono::high_resolution_clock::now() - m_start_time;
            duration_hours = std::chrono::duration_cast<std::chrono::hours>(duration).count() % 24;
            duration_minutes = std::chrono::duration_cast<std::chrono::minutes>(duration).count() % 60;
            duration_seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count() % 60;
        }

        std::string hours_str = "00";
        std::string minutes_str = "00";
        std::string seconds_str = "00";

        if (duration_hours > 0)
        {
            hours_str = (duration_hours < 10 ? "0" : "") + std::to_string(duration_hours);
        }

        if (duration_minutes > 0)
        {
            minutes_str = (duration_minutes < 10 ? "0" : "") + std::to_string(duration_minutes);
        }

        if (duration_seconds > 0)
        {
            seconds_str = (duration_seconds < 10 ? "0" : "") + std::to_string(duration_seconds);
        }

        status_payload["uptime"] = hours_str + ":" + minutes_str + ":" + seconds_str;

        if (cpu_usage != nullptr)
        {
            status_payload["cpuUsage"] = os_cpu_usage_info_query(cpu_usage);
        }

        char *current_profile = obs_frontend_get_current_profile();
        if (current_profile != nullptr)
        {
            status_payload["activeProfile"] = std::string(current_profile);
            bfree(current_profile);
        }

        char *current_collection = obs_frontend_get_current_scene_collection();
        if (current_collection != nullptr)
        {
            status_payload["activeCollection"] = std::string(current_collection);
            bfree(current_collection);
        }

        obs_source_t *current_scene = obs_frontend_get_current_scene();
        if (current_scene != nullptr)
        {
            const char *scene_name = obs_source_get_name(current_scene);
            if (scene_name != nullptr)
            {
                status_payload["activeScene"] = std::string(scene_name);
            }
            obs_source_release(current_scene);
        }

        send_message(new_status);
        
        // Send analytics reports if banner manager is enabled
        if constexpr (BANNER_MANAGER_ENABLED) {
            // Update viewer metrics for analytics
            int estimated_viewers = 0;
            // Estimate viewers based on stream metrics (simplified)
            if (status_payload["currentState"] == "STREAMING" && bps > 0) {
                estimated_viewers = static_cast<int>(bps / 100); // Rough estimate based on bitrate
            }
            
            // Viewer metrics tracking removed - simplified banner system
            
            // Analytics reporting removed - simplified banner system
        }
    }

    // Cleanup
    if (cpu_usage != nullptr)
    {
        os_cpu_usage_info_destroy(cpu_usage);
    }
}
void vorti::applets::obs_plugin::create_obs_menu()
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        m_banner_manager.add_banner_menu();
        
        // CRITICAL: Initialize banners after OBS is fully loaded
        // This prevents the "sources could not be unloaded" error
        m_banner_manager.initialize_after_obs_ready();
    }
    
    // Create top-level VortiDeck menu (replaces individual Tools menu items)
    create_vortideck_menu();
}

// Menu callbacks are now handled by the banner manager

// Banner action implementations
void vorti::applets::obs_plugin::action_banner_show(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        log_to_obs("ACTION_BANNER_SHOW: Showing banner");
        m_banner_manager.show_banner();
    }
}

void vorti::applets::obs_plugin::action_banner_hide(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        log_to_obs("ACTION_BANNER_HIDE: Hiding banner");
        m_banner_manager.hide_banner();
    }
}

void vorti::applets::obs_plugin::action_banner_toggle(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        log_to_obs("ACTION_BANNER_TOGGLE: Toggling banner");
        m_banner_manager.toggle_banner();
    }
}


// ============================================================================
// OVERLAY ACTION HANDLERS (NO RESTRICTIONS)
// ============================================================================

void vorti::applets::obs_plugin::action_overlay_set_data(const action_invoke_parameters &parameters)
{
    auto url_it = parameters.find("url");
    
    if (url_it != parameters.end()) {
        std::string url = url_it->second;
        
        // Update ALL existing overlay sources with the new URL
        obs_enum_sources([](void* data, obs_source_t* source) {
            std::string url = *(std::string*)data;
            
            // Check if this is a VortiDeck overlay source
            const char* source_id = obs_source_get_id(source);
            if (source_id && strcmp(source_id, vortideck::SOURCE_ID_OVERLAY) == 0) {
                // Update the overlay URL
                obs_data_t* settings = obs_source_get_settings(source);
                obs_data_set_string(settings, "url", url.c_str());
                obs_source_update(source, settings);
                obs_data_release(settings);
                
                const char* name = obs_source_get_name(source);
                blog(LOG_INFO, "ACTION_OVERLAY_SET_DATA: Updated overlay '%s' with new URL", 
                     name ? name : "unnamed");
            }
            return true; // Continue enumeration
        }, &url);
        
        log_to_obs("ACTION_OVERLAY_SET_DATA: Updated all existing overlays with URL: " + url);
        
        std::string overlay_name = "VortiDeck Overlay";
        
        // Find or create the main overlay source
        obs_source_t* overlay_source = obs_get_source_by_name(overlay_name.c_str());
        
        if (!overlay_source) {
            // Create main overlay source (full canvas size)
            obs_data_t* settings = obs_data_create();
            obs_data_set_string(settings, "url", url.c_str());
            
            // Full canvas dimensions
            auto canvas_info = get_current_canvas_info();
            int canvas_width = 1920;  // Default
            int canvas_height = 1080; // Default
            
            if (canvas_info.contains("width")) {
                canvas_width = canvas_info["width"];
            }
            if (canvas_info.contains("height")) {
                canvas_height = canvas_info["height"];
            }
            
            obs_data_set_int(settings, "width", canvas_width);
            obs_data_set_int(settings, "height", canvas_height);
            obs_data_set_string(settings, "overlay_id", "main_overlay");
            obs_data_set_bool(settings, "auto_resize", true); // Enable auto-resize for main overlay
            
            overlay_source = obs_source_create(vortideck::SOURCE_ID_OVERLAY, overlay_name.c_str(), settings, nullptr);
            obs_data_release(settings);
            
            if (overlay_source) {
                log_to_obs("ACTION_OVERLAY_SET_DATA: Created main overlay source (full canvas)");
                
                // Add to all scenes like banner system
                obs_frontend_source_list scenes = {};
                obs_frontend_get_scenes(&scenes);
                
                for (size_t i = 0; i < scenes.sources.num; i++) {
                    obs_source_t* scene_source = scenes.sources.array[i];
                    if (!scene_source) continue;
                    
                    obs_scene_t* scene = obs_scene_from_source(scene_source);
                    if (!scene) continue;
                    
                    obs_sceneitem_t* scene_item = obs_scene_add(scene, overlay_source);
                    if (scene_item) {
                        // Position at 0,0 (full screen overlay)
                        vec2 pos = {0.0f, 0.0f};
                        obs_sceneitem_set_pos(scene_item, &pos);
                        
                        // Set bounds to canvas size for stretching
                        vec2 bounds = {(float)canvas_width, (float)canvas_height};
                        obs_sceneitem_set_bounds(scene_item, &bounds);
                        obs_sceneitem_set_bounds_type(scene_item, OBS_BOUNDS_STRETCH);
                        obs_sceneitem_set_bounds_alignment(scene_item, 0); // Top-left
                        
                        // Lock the main overlay to prevent user manipulation
                        obs_sceneitem_set_locked(scene_item, true);
                        
                        // Move to bottom so ADS stays on top
                        obs_sceneitem_set_order(scene_item, OBS_ORDER_MOVE_BOTTOM);
                    }
                }
                
                obs_frontend_source_list_free(&scenes);
            } else {
                log_to_obs("ACTION_OVERLAY_SET_DATA: ERROR - Failed to create overlay source");
                return;
            }
        } else {
            // Update existing overlay URL and ensure auto-resize stays enabled
            obs_data_t* settings = obs_source_get_settings(overlay_source);
            obs_data_set_string(settings, "url", url.c_str());
            obs_data_set_bool(settings, "auto_resize", true); // Ensure auto-resize stays enabled
            
            // Update canvas size
            auto canvas_info = get_current_canvas_info();
            if (canvas_info.contains("width") && canvas_info.contains("height")) {
                obs_data_set_int(settings, "width", canvas_info["width"]);
                obs_data_set_int(settings, "height", canvas_info["height"]);
            }
            
            obs_source_update(overlay_source, settings);
            obs_data_release(settings);
            
            log_to_obs("ACTION_OVERLAY_SET_DATA: Updated main overlay URL and ensured auto-resize");
        }
        
        if (overlay_source) {
            obs_source_release(overlay_source);
        }
        
        log_to_obs("ACTION_OVERLAY_SET_DATA: Overlay data set successfully - URL: " + url);
    } else {
        log_to_obs("ACTION_OVERLAY_SET_DATA: ERROR - Missing required parameter (url)");
    }
}

void vorti::applets::obs_plugin::action_overlay_create(const action_invoke_parameters &parameters)
{
    auto overlay_id_it = parameters.find("overlay_id");
    auto url_it = parameters.find("url");
    auto width_it = parameters.find("width");
    auto height_it = parameters.find("height");
    auto name_it = parameters.find("name");
    auto scene_name_it = parameters.find("scene_name");
    
    if (overlay_id_it != parameters.end() && url_it != parameters.end()) {
        std::string overlay_id = overlay_id_it->second;
        std::string url = url_it->second;
        std::string source_name = name_it != parameters.end() ? name_it->second : ("VortiDeck Overlay " + overlay_id);
        std::string scene_name = scene_name_it != parameters.end() ? scene_name_it->second : "";
        
        int width = width_it != parameters.end() ? std::stoi(width_it->second) : 1920;
        int height = height_it != parameters.end() ? std::stoi(height_it->second) : 1080;
        
        // Create overlay source settings
        obs_data_t* settings = obs_data_create();
        obs_data_set_string(settings, "overlay_id", overlay_id.c_str());
        obs_data_set_string(settings, "url", url.c_str());
        obs_data_set_int(settings, "width", width);
        obs_data_set_int(settings, "height", height);
        
        // Create the overlay source
        obs_source_t* overlay_source = obs_source_create(vortideck::SOURCE_ID_OVERLAY, source_name.c_str(), settings, nullptr);
        
        if (overlay_source) {
            // Add to scene if specified
            if (!scene_name.empty()) {
                obs_source_t* scene = obs_get_source_by_name(scene_name.c_str());
                if (scene) {
                    obs_scene_t* obs_scene = obs_scene_from_source(scene);
                    if (obs_scene) {
                        obs_sceneitem_t* scene_item = obs_scene_add(obs_scene, overlay_source);
                        if (scene_item) {
                            log_to_obs("ACTION_OVERLAY_CREATE: Created overlay '" + source_name + "' and added to scene '" + scene_name + "'");
                        }
                    }
                    obs_source_release(scene);
                }
            } else {
                log_to_obs("ACTION_OVERLAY_CREATE: Created overlay source '" + source_name + "'");
            }
            obs_source_release(overlay_source);
        } else {
            log_to_obs("ACTION_OVERLAY_CREATE: ERROR - Failed to create overlay source");
        }
        
        obs_data_release(settings);
    } else {
        log_to_obs("ACTION_OVERLAY_CREATE: ERROR - Missing required parameters (overlay_id and url)");
    }
}

void vorti::applets::obs_plugin::action_overlay_update(const action_invoke_parameters &parameters)
{
    auto source_name_it = parameters.find("source_name");
    auto url_it = parameters.find("url");
    auto width_it = parameters.find("width");
    auto height_it = parameters.find("height");
    
    if (source_name_it != parameters.end()) {
        std::string source_name = source_name_it->second;
        obs_source_t* source = obs_get_source_by_name(source_name.c_str());
        
        if (source && strcmp(obs_source_get_id(source), vortideck::SOURCE_ID_OVERLAY) == 0) {
            obs_data_t* settings = obs_source_get_settings(source);
            
            // Check if dimensions are changing (indicates VortiDeck resolution update)
            bool dimensions_changed = false;
            if (width_it != parameters.end() || height_it != parameters.end()) {
                int current_width = (int)obs_data_get_int(settings, "width");
                int current_height = (int)obs_data_get_int(settings, "height");
                
                int new_width = (width_it != parameters.end()) ? std::stoi(width_it->second) : current_width;
                int new_height = (height_it != parameters.end()) ? std::stoi(height_it->second) : current_height;
                
                log_to_obs("ACTION_OVERLAY_UPDATE: Comparing dimensions - current: " + std::to_string(current_width) + "x" + std::to_string(current_height) + 
                          ", new: " + std::to_string(new_width) + "x" + std::to_string(new_height));
                
                if (new_width != current_width || new_height != current_height) {
                    dimensions_changed = true;
                    log_to_obs("ACTION_OVERLAY_UPDATE: Dimensions changing from " + std::to_string(current_width) + "x" + std::to_string(current_height) + 
                              " to " + std::to_string(new_width) + "x" + std::to_string(new_height));
                } else {
                    log_to_obs("ACTION_OVERLAY_UPDATE: No dimension change detected, but forcing recreation anyway for VortiDeck content update");
                    dimensions_changed = true; // Force recreation even without dimension change for content updates
                }
            }
            
            if (url_it != parameters.end()) {
                obs_data_set_string(settings, "url", url_it->second.c_str());
            }
            if (width_it != parameters.end()) {
                obs_data_set_int(settings, "width", std::stoi(width_it->second));
            }
            if (height_it != parameters.end()) {
                obs_data_set_int(settings, "height", std::stoi(height_it->second));
            }
            
            // Add flag to trigger browser source recreation for dimension changes
            if (dimensions_changed) {
                obs_data_set_bool(settings, "force_browser_recreation", true);
                log_to_obs("ACTION_OVERLAY_UPDATE: Flagging for browser source recreation");
            }
            
            obs_source_update(source, settings);
            obs_data_release(settings);
            
            log_to_obs("ACTION_OVERLAY_UPDATE: Updated overlay source '" + source_name + "'" + 
                      (dimensions_changed ? " (recreating browser)" : ""));
        } else {
            log_to_obs("ACTION_OVERLAY_UPDATE: ERROR - Source '" + source_name + "' not found or not a VortiDeck overlay");
        }
        
        if (source) obs_source_release(source);
    } else {
        log_to_obs("ACTION_OVERLAY_UPDATE: ERROR - Missing required parameter (source_name)");
    }
}

void vorti::applets::obs_plugin::action_overlay_remove(const action_invoke_parameters &parameters)
{
    auto source_name_it = parameters.find("source_name");
    
    if (source_name_it != parameters.end()) {
        std::string source_name = source_name_it->second;
        obs_source_t* source = obs_get_source_by_name(source_name.c_str());
        
        if (source && strcmp(obs_source_get_id(source), vortideck::SOURCE_ID_OVERLAY) == 0) {
            obs_source_remove(source);
            log_to_obs("ACTION_OVERLAY_REMOVE: Removed overlay source '" + source_name + "'");
        } else {
            log_to_obs("ACTION_OVERLAY_REMOVE: ERROR - Source '" + source_name + "' not found or not a VortiDeck overlay");
        }
        
        if (source) obs_source_release(source);
    } else {
        log_to_obs("ACTION_OVERLAY_REMOVE: ERROR - Missing required parameter (source_name)");
    }
}

// ============================================================================
// CONTINUOUS mDNS DISCOVERY IMPLEMENTATION
// ============================================================================

void vorti::applets::obs_plugin::start_continuous_discovery()
{
    if (m_continuous_discovery_thread.joinable()) {
        return; // Already running
    }
    
    log_to_obs("Starting continuous mDNS discovery thread");
    m_continuous_discovery_enabled = true;
    
    // Initialize mDNS discovery if needed
    if (!m_mdns_discovery) {
        m_mdns_discovery = std::make_unique<MDNSDiscovery>();
    }
    
    // Start the continuous discovery thread
    m_continuous_discovery_thread = std::jthread([]() { continuous_discovery_worker(); });
}

void vorti::applets::obs_plugin::stop_continuous_discovery()
{
    log_to_obs("Stopping continuous mDNS discovery");
    m_continuous_discovery_enabled = false;

    // CRITICAL: Stop mDNS discovery first to interrupt any ongoing discovery
    if (m_mdns_discovery) {
        m_mdns_discovery->stop_discovery();
    }

    // Request thread stop - it will check m_shutting_down and exit
    if (m_continuous_discovery_thread.joinable()) {
        m_continuous_discovery_thread.request_stop();

        // DON'T join or detach - just let it exit when it checks flags
        // The process will terminate soon anyway
        log_to_obs("Requested mDNS discovery thread to stop");
    }
}

void vorti::applets::obs_plugin::continuous_discovery_worker()
{
    log_to_obs("Continuous mDNS discovery worker started");
    
    while (m_continuous_discovery_enabled.load() && !m_shutting_down) {
        try {
            // Perform discovery every 30 seconds or when no service is found
            bool should_discover = false;
            
            {
                std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
                auto now = std::chrono::steady_clock::now();
                
                // Discover if no services found or it's been 30+ seconds since last discovery
                should_discover = m_discovered_services.empty() || 
                                 (now - m_last_discovery_time) > std::chrono::seconds(30);
            }
            
            if (should_discover) {
                log_to_obs("Starting background mDNS discovery for VortiDeck services");
                
                // Use the safer synchronous discovery approach instead of async
                // Check if discovery is already in progress to avoid conflicts
                if (m_mdns_discovery && !m_mdns_discovery->is_discovering()) {
                    try {
                        // Use synchronous discovery with short timeout to avoid crashes
                        auto services = m_mdns_discovery->discover_services(
                            std::chrono::seconds(5), // Short 5 second timeout
                            false
                        );
                        
                        // Process discovered services
                        for (const auto& service : services) {
                            on_service_discovered(service);
                        }
                        
                        // Update last discovery time
                        {
                            std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
                            m_last_discovery_time = std::chrono::steady_clock::now();
                        }
                        
                        if (!services.empty()) {
                            log_to_obs("Found VortiDeck services via mDNS discovery");
                        } else {
                            log_to_obs("No VortiDeck services found via mDNS discovery");
                        }
                        
                    } catch (const std::exception& e) {
                        log_to_obs(std::format("Background mDNS discovery error: {}", e.what()).c_str());
                    }
                } else {
                    log_to_obs("Skipping background discovery - already in progress");
                }
                
                // Update last discovery time to prevent repeated messages
                {
                    std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
                    m_last_discovery_time = std::chrono::steady_clock::now();
                }
            }
            
            // Sleep for 5 seconds before next check
            for (int i = 0; i < 50 && m_continuous_discovery_enabled.load() && !m_shutting_down; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            log_to_obs(std::format("Error in continuous discovery: {}", e.what()));
            // Sleep longer on error
            for (int i = 0; i < 100 && m_continuous_discovery_enabled.load() && !m_shutting_down; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    
    log_to_obs("Continuous mDNS discovery worker stopped");
}

void vorti::applets::obs_plugin::save_discovered_service_state(const ServiceInfo& service)
{
    log_to_obs(std::format("Saving service state: {}", service.websocket_url));
    
    // Save to member variables for immediate use
    m_last_known_service_url = service.websocket_url;
    m_last_known_service_ip = service.ip_address;
    m_last_known_service_port = service.port;
    
    // TODO: Could save to file/registry for persistence across OBS restarts
    // For now, just keep in memory during OBS session
}

bool vorti::applets::obs_plugin::load_last_known_service_state()
{
    // Check if we have a previously discovered service in memory
    if (!m_last_known_service_url.empty() && 
        !m_last_known_service_ip.empty() && 
        m_last_known_service_port > 0) {
        
        m_discovered_websocket_url = m_last_known_service_url;
        log_to_obs(std::format("Loaded last known service: {}", m_last_known_service_url));
        return true;
    }
    
    // TODO: Could load from file/registry
    return false;
}

std::string vorti::applets::obs_plugin::get_best_available_service_url()
{
    bool should_show_dialog = false;
    size_t service_count = 0;
    
    // Check if we should show dialog (scope the mutex lock)
    {
        std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
        service_count = m_discovered_services.size();
        // Only show dialog for multiple services (not single service)
        should_show_dialog = (service_count > 1 && !m_show_selection_dialog.exchange(true));
    }
    
    // Show dialog outside of mutex lock to avoid deadlock
    if (should_show_dialog) {
        // Check if a manual dialog is already open - if so, skip auto-dialog entirely
        {
            std::lock_guard<std::mutex> dialog_lock(m_dialog_mutex);
            if (m_dialog_is_open) {
                log_to_obs("Manual dialog already open - auto-connection will proceed without dialog");
                should_show_dialog = false;
 
            }
        }
        
        if (should_show_dialog) {
            log_to_obs(std::format("Multiple VortiDeck services discovered ({})", service_count));
            log_to_obs("DEBUG: About to call show_service_selection_dialog() directly");
            
            // AUTO-CONNECTION NEVER SHOWS DIALOGS - just proceed to connect
            
            log_to_obs("DEBUG: show_service_selection_dialog() call completed");
        }
    } else if (service_count > 1) {
        log_to_obs("DEBUG: Multiple services but selection dialog already shown or manual dialog open");
    }
    
    // Now get the service URL with a fresh mutex lock
    {
        std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
    
        // If user has selected a specific service, use that
        if (!m_selected_service_url.empty()) {
            return m_selected_service_url;
        }
        
        // Use the most recently discovered service if available
        if (!m_discovered_services.empty()) {
            const auto& service = m_discovered_services.back();
            log_to_obs(std::format("Using discovered service: {}", service.websocket_url));
            // Store the selected service URL for status tracking
            m_selected_service_url = service.websocket_url;
            log_to_obs(std::format("Auto-connection stored service URL: {}", m_selected_service_url));
            return service.websocket_url;
        }
        
        // Use cached discovery URL if available
        if (!m_discovered_websocket_url.empty()) {
            log_to_obs(std::format("Using cached service: {}", m_discovered_websocket_url));
            // Store the selected service URL for status tracking
            m_selected_service_url = m_discovered_websocket_url;
            log_to_obs(std::format("Auto-connection stored cached URL: {}", m_selected_service_url));
            return m_discovered_websocket_url;
        }
        
        // Use last known service if available
        if (!m_last_known_service_url.empty()) {
            log_to_obs(std::format("Using last known service: {}", m_last_known_service_url));
            // Store the selected service URL for status tracking
            m_selected_service_url = m_last_known_service_url;
            log_to_obs(std::format("Auto-connection stored last known URL: {}", m_selected_service_url));
            return m_last_known_service_url;
        }
        
        // No service found - this will cause connection to fail, triggering retry
        log_to_obs("No VortiDeck service found - waiting for discovery");
        return "";
    } // Close mutex scope
}

void vorti::applets::obs_plugin::show_service_selection_dialog(bool force_show_dialog)
{
    log_to_obs("DEBUG: show_service_selection_dialog() STARTED");
    
    // Simple RAII dialog guard - automatically resets state on function exit
    struct DialogGuard {
        bool& flag;
        std::mutex& mutex;
        bool valid;
        
        DialogGuard(bool& f, std::mutex& m) : flag(f), mutex(m), valid(false) {
            std::lock_guard<std::mutex> lock(mutex);
            if (flag) {
                log_to_obs("Dialog already open - preventing duplicate dialog");
                return; // valid stays false
            }
            flag = true;
            valid = true;
        }
        
        ~DialogGuard() {
            if (valid) {
                std::lock_guard<std::mutex> lock(mutex);
                flag = false;
            }
        }
    };
    
    DialogGuard guard(m_dialog_is_open, m_dialog_mutex);
    if (!guard.valid) return;
    
    // Prevent dialog from opening during discovery to avoid crashes
    if (m_discovery_in_progress && !force_show_dialog) {
        log_to_obs("⏳ Discovery in progress - deferring dialog until complete");
        return;
    }
    
    std::vector<ServiceInfo> services_copy;
    {
        std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
        services_copy = m_discovered_services;
        log_to_obs(std::format("DEBUG: Copied {} services from m_discovered_services", services_copy.size()));
    }
    
    if (services_copy.empty()) {
        log_to_obs("DEBUG: services_copy is empty, returning early");
        return;
    }
    
    // Show clear service selection information in log first
    log_to_obs("==========================================");
    log_to_obs("🔍 MULTIPLE VORTIDECK SERVICES DISCOVERED");
    log_to_obs("==========================================");
    log_to_obs("");
    log_to_obs("Found multiple VortiDeck services on your network:");
    log_to_obs("");
    
    for (size_t i = 0; i < services_copy.size(); ++i) {
        const auto& service = services_copy[i];
        log_to_obs(std::format("📱 SERVICE {}: {}", i + 1, service.name));
        log_to_obs(std::format("   🌐 IP Address: {}", service.ip_address));
        log_to_obs(std::format("   🔌 Port: {}", service.port));
        log_to_obs(std::format("   🔗 WebSocket URL: {}", service.websocket_url));
        log_to_obs("");
    }
    
    // Show Qt6 dialog for service selection
    try {
        log_to_obs("🖥️ SHOWING SERVICE SELECTION DIALOG WINDOW...");
        
        // Use a simple check for frontend readiness
        static bool frontend_ready = true;  // Assume ready since we're in post_load phase
        if (!frontend_ready) {
            log_to_obs("⏳ OBS frontend not ready yet - deferring dialog creation");
            // Use automatic selection for now
            int preferred_index = -1;
            for (size_t i = 0; i < services_copy.size(); ++i) {
                if (services_copy[i].port == 9001) {
                    preferred_index = static_cast<int>(i);
                    break;
                }
            }
            
            if (preferred_index >= 0) {
                const auto& preferred_service = services_copy[preferred_index];
                m_selected_service_url = preferred_service.websocket_url;
                log_to_obs(std::format("✅ Auto-selected: {} (Port {})", preferred_service.name, preferred_service.port));
            } else {
                const auto& fallback_service = services_copy[0];
                m_selected_service_url = fallback_service.websocket_url;
                log_to_obs(std::format("⚠️ Auto-selected: {} (Port {})", fallback_service.name, fallback_service.port));
            }
            return;
        }
        
        // Use OBS's main window as parent (like countdown plugin does)
        void* main_window = obs_frontend_get_main_window();
        if (!main_window) {
            log_to_obs("❌ No OBS main window found - using automatic selection");
            // Fallback to automatic selection
            int preferred_index = -1;
            for (size_t i = 0; i < services_copy.size(); ++i) {
                if (services_copy[i].port == 9001) {  // Real VortiDeck service
                    preferred_index = static_cast<int>(i);
                    break;
                }
            }
            
            if (preferred_index >= 0) {
                const auto& preferred_service = services_copy[preferred_index];
                m_selected_service_url = preferred_service.websocket_url;
                log_to_obs(std::format("✅ Auto-selected: {} (Port {})", preferred_service.name, preferred_service.port));
            } else {
                const auto& fallback_service = services_copy[0];
                m_selected_service_url = fallback_service.websocket_url;
                log_to_obs(std::format("⚠️ Auto-selected: {} (Port {})", fallback_service.name, fallback_service.port));
            }
            return;
        }
        
#if 1 // Enable Qt dialog
        // Check if we have Qt application
        log_to_obs("DEBUG: About to check Qt application");
        QApplication* app = qApp;
        log_to_obs(std::format("DEBUG: QApplication pointer: {}", app ? "valid" : "null"));
        
        if (!app) {
            log_to_obs("ERROR: No Qt application found - cannot show dialog");
            return;
        }
        
        // Create and show dialog on main thread using Qt's thread-safe mechanism
        log_to_obs("DEBUG: About to create ServiceSelectionDialog on main thread");
        log_to_obs(std::format("DEBUG: main_window pointer: {}", main_window ? "valid" : "null"));
        
        int result = QDialog::Rejected;
        std::string selectedUrl;
        int selectedIndex = -1;
        bool dialog_completed = false;
        
        // Check if we're already on the main thread to avoid deadlock
        bool on_main_thread = (QThread::currentThread() == app->thread());
        log_to_obs(std::format("DEBUG: Running on main thread: {}", on_main_thread ? "YES" : "NO"));
        
        // For manual dialogs (force_show_dialog=true), use simple non-blocking approach
        if (force_show_dialog) {
            log_to_obs("DEBUG: Manual dialog requested - creating simple non-blocking dialog");
            
            // Get current connection status to pass to dialog
            std::string current_connected_url = m_selected_service_url;
            bool is_currently_connected = is_connected();
            
            log_to_obs(std::format("DEBUG: Current connection status - Connected: {}, URL: {}", 
                is_currently_connected ? "YES" : "NO", current_connected_url));
            
            // Simple non-blocking dialog creation with connection status
            QMetaObject::invokeMethod(app, [services_copy, main_window, current_connected_url, is_currently_connected, persistent_dialog_ptr = &m_persistent_dialog, dialog_mutex_ptr = &m_dialog_mutex]() {
                log_to_obs("DEBUG: Creating simple manual dialog with connection status");
                ServiceSelectionDialog* dialog = new ServiceSelectionDialog(services_copy, static_cast<QWidget*>(main_window));
                
                // Store dialog reference for live updates
                {
                    std::lock_guard<std::mutex> lock(*dialog_mutex_ptr);
                    *persistent_dialog_ptr = static_cast<void*>(dialog);
                }
                
                // Mark the connected service if we have one
                if (is_currently_connected && !current_connected_url.empty()) {
                    log_to_obs(std::format("DEBUG: Looking for connected URL '{}' in {} services", current_connected_url, services_copy.size()));
                    
                    bool found_match = false;
                    for (size_t i = 0; i < services_copy.size(); ++i) {
                        log_to_obs(std::format("DEBUG: Service {}: {} (URL: '{}')", i, services_copy[i].name, services_copy[i].websocket_url));
                        
                        if (services_copy[i].websocket_url == current_connected_url) {
                            log_to_obs(std::format("DEBUG: MATCH FOUND! Marking service {} as connected", i));
                            dialog->markServiceAsConnected(static_cast<int>(i));
                            log_to_obs(std::format("Manual dialog shows service {} as connected", services_copy[i].name));
                            found_match = true;
                            break;
                        }
                    }
                    
                    if (!found_match) {
                        log_to_obs("DEBUG: NO MATCH FOUND - connected URL not in service list");
                        log_to_obs(std::format("DEBUG: Connected URL: '{}'", current_connected_url));
                        log_to_obs("DEBUG: Available services:");
                        for (size_t i = 0; i < services_copy.size(); ++i) {
                            log_to_obs(std::format("DEBUG:   [{}] {} -> '{}'", i, services_copy[i].name, services_copy[i].websocket_url));
                        }
                    }
                } else {
                    log_to_obs(std::format("DEBUG: Not marking any service as connected - Connected: {}, URL: '{}'", 
                        is_currently_connected ? "true" : "false", current_connected_url));
                }
                
                // Connect cleanup when dialog closes
                QObject::connect(dialog, &QDialog::finished, [persistent_dialog_ptr, dialog_mutex_ptr]() {
                    std::lock_guard<std::mutex> lock(*dialog_mutex_ptr);
                    *persistent_dialog_ptr = nullptr;
                    log_to_obs("DEBUG: Manual dialog closed - reference cleared for live updates");
                });
                
                dialog->setAttribute(Qt::WA_DeleteOnClose); // Auto-delete when closed
                dialog->show();
                dialog->raise();
                dialog->activateWindow();
                log_to_obs("DEBUG: Simple manual dialog shown (non-blocking) with live update support");
            });
            
            log_to_obs("DEBUG: Simple manual dialog creation queued");
            return;
        }
        
        // For auto-dialogs (should never happen now), use blocking approach
        if (false) {
            // We're already on the main thread, call directly
            log_to_obs("DEBUG: Creating dialog directly (already on main thread)");
            ServiceSelectionDialog dialog(services_copy, static_cast<QWidget*>(main_window));
            log_to_obs("DEBUG: ServiceSelectionDialog created successfully");
            
            // Store dialog reference for live updates
            {
                std::lock_guard<std::mutex> lock(m_dialog_mutex);
                m_persistent_dialog = static_cast<void*>(&dialog);
            }
            
            // Update dialog with current connection status if we're already connected
            if (is_connected() && !m_selected_service_url.empty()) {
                for (size_t i = 0; i < services_copy.size(); ++i) {
                    if (services_copy[i].websocket_url == m_selected_service_url) {
                        dialog.markServiceAsConnected(static_cast<int>(i));
                        log_to_obs(std::format("Dialog initialized with connection status - service {} is connected", services_copy[i].name));
                        break;
                    }
                }
            }
            
            // Connect refresh signal to update connection status
            QObject::connect(&dialog, &ServiceSelectionDialog::refreshRequested, [&]() {
                log_to_obs("Dialog refresh requested - updating connection status");
                
                // Refresh service discovery
                if (!m_discovery_in_progress) {
                    start_continuous_discovery();
                }
                
                // Update dialog with current connection status
                if (is_connected() && !m_selected_service_url.empty()) {
                    std::vector<ServiceInfo> current_services;
                    {
                        std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
                        current_services = m_discovered_services;
                    }
                    
                    dialog.updateServiceList(current_services);
                    
                    // Mark connected service
                    for (size_t i = 0; i < current_services.size(); ++i) {
                        if (current_services[i].websocket_url == m_selected_service_url) {
                            dialog.markServiceAsConnected(static_cast<int>(i));
                            log_to_obs(std::format("Refresh updated - service {} is connected", current_services[i].name));
                            break;
                        }
                    }
                }
            });
            log_to_obs("DEBUG: About to call dialog.exec()");
            result = dialog.exec();
            log_to_obs(std::format("DEBUG: dialog.exec() returned: {}", result));
            
            if (result == QDialog::Accepted) {
                selectedUrl = dialog.getSelectedServiceUrl();
                selectedIndex = dialog.getSelectedServiceIndex();
                log_to_obs("DEBUG: Retrieved dialog results");
            }
            
            // Clear persistent dialog reference
            m_persistent_dialog = nullptr;
            
            dialog_completed = true;
        } else {
            // We're on a background thread, use invokeMethod
            QMetaObject::invokeMethod(app, [&]() {
                log_to_obs("DEBUG: Creating dialog on main thread via invokeMethod");
                ServiceSelectionDialog dialog(services_copy, static_cast<QWidget*>(main_window));
                log_to_obs("DEBUG: ServiceSelectionDialog created successfully on main thread");
                log_to_obs("DEBUG: About to show dialog (non-blocking) on main thread");
                dialog.show();
                dialog.raise();
                dialog.activateWindow();
                log_to_obs("DEBUG: Dialog shown (non-blocking) on main thread");
                
                // Note: Dialog will handle its own results via signals, no blocking needed
                result = QDialog::Accepted; // Assume success for now
                
                if (result == QDialog::Accepted) {
                    selectedUrl = dialog.getSelectedServiceUrl();
                    selectedIndex = dialog.getSelectedServiceIndex();
                    log_to_obs("DEBUG: Retrieved dialog results on main thread");
                }
                dialog_completed = true;
            }, Qt::BlockingQueuedConnection);
        }
        
        if (!dialog_completed) {
            log_to_obs("ERROR: Dialog execution failed or timed out");
            return;
        }
        
        if (result == QDialog::Accepted) {
            
            if (!selectedUrl.empty() && selectedIndex >= 0) {
                // Store the selected service
                m_selected_service_url = selectedUrl;
                const auto& selected_service = services_copy[selectedIndex];
                log_to_obs("✅ USER SELECTION CONFIRMED:");
                log_to_obs(std::format("   Selected: {} (Port {})", selected_service.name, selected_service.port));
                log_to_obs(std::format("   URL: {}", selectedUrl));
                
                // Save this choice for future sessions
                // TODO: Implement persistent storage
                
            } else {
                log_to_obs("❌ Dialog accepted but no valid service selected");
            }
        } else {
            log_to_obs("⚠️ USER CANCELLED SELECTION - using automatic selection");
            
            // User cancelled - use automatic selection as fallback
            int preferred_index = -1;
            for (size_t i = 0; i < services_copy.size(); ++i) {
                if (services_copy[i].port == 9001) {  // Real VortiDeck service
                    preferred_index = static_cast<int>(i);
                    break;
                }
            }
            
            if (preferred_index >= 0) {
                const auto& preferred_service = services_copy[preferred_index];
                m_selected_service_url = preferred_service.websocket_url;
                log_to_obs(std::format("✅ Auto-selected: {} (Port {})", preferred_service.name, preferred_service.port));
            } else {
                const auto& fallback_service = services_copy[0];
                m_selected_service_url = fallback_service.websocket_url;
                log_to_obs(std::format("⚠️ Auto-selected: {} (Port {})", fallback_service.name, fallback_service.port));
            }
        }
#endif // End Qt dialog disable
        
    } catch (const std::exception& e) {
        log_to_obs(std::format("❌ Exception showing service selection dialog: {}", e.what()));
        log_to_obs("Using automatic selection as fallback...");
        
        // Clear persistent dialog reference on exception
        m_persistent_dialog = nullptr;
        
        // Exception occurred - use automatic selection as fallback
        int preferred_index = -1;
        for (size_t i = 0; i < services_copy.size(); ++i) {
            if (services_copy[i].port == 9001) {  // Real VortiDeck service
                preferred_index = static_cast<int>(i);
                break;
            }
        }
        
        if (preferred_index >= 0) {
            const auto& preferred_service = services_copy[preferred_index];
            m_selected_service_url = preferred_service.websocket_url;
            log_to_obs(std::format("✅ Fallback: {} (Port {})", preferred_service.name, preferred_service.port));
        } else {
            const auto& fallback_service = services_copy[0];
            m_selected_service_url = fallback_service.websocket_url;
            log_to_obs(std::format("⚠️ Fallback: {} (Port {})", fallback_service.name, fallback_service.port));
        }
    } catch (...) {
        log_to_obs("❌ Unknown exception showing service selection dialog");
        log_to_obs("Using automatic selection as fallback...");
        
        // Clear persistent dialog reference on exception
        m_persistent_dialog = nullptr;
        
        // Use first service as ultimate fallback
        if (!services_copy.empty()) {
            const auto& fallback_service = services_copy[0];
            m_selected_service_url = fallback_service.websocket_url;
            log_to_obs(std::format("⚠️ Ultimate fallback: {} (Port {})", fallback_service.name, fallback_service.port));
        }
    }
    
    log_to_obs("==========================================");
    log_to_obs("DEBUG: show_service_selection_dialog() COMPLETED");
}

std::string vorti::applets::obs_plugin::select_service_from_dialog(const std::vector<ServiceInfo>& services)
{
    // This function would be called from a UI dialog in the future
    // For now, return empty to use default selection logic
    return "";
}

void vorti::applets::obs_plugin::create_vortideck_menu()
{
    log_to_obs("Creating top-level VortiDeck menu...");
    
    // Get OBS main window
    auto main_window = static_cast<QMainWindow*>(obs_frontend_get_main_window());
    if (!main_window) {
        log_to_obs("ERROR: Could not get main window for VortiDeck menu creation");
        return;
    }
    
    QMenuBar* menu_bar = main_window->menuBar();
    if (!menu_bar) {
        log_to_obs("ERROR: Could not get menu bar");
        return;
    }
    
    // Create the top-level VortiDeck menu
    QMenu* vortideck_menu = menu_bar->addMenu("VortiDeck");
    
    // Add Banner Settings submenu item
    QAction* banner_action = vortideck_menu->addAction("Banner Settings (ADS)");
    QObject::connect(banner_action, &QAction::triggered, []() {
        log_to_obs("VortiDeck Banner Settings (ADS) clicked from top-level menu");
        // Open VortiDeck banner settings page
        DeepLinkHandler::open_vortideck_with_fallback("banner-settings");
    });
    
    // Add separator
    vortideck_menu->addSeparator();
    
    // Add Overlays menu item (no restrictions)
    QAction* overlays_action = vortideck_menu->addAction("Overlays (Free)");
    QObject::connect(overlays_action, &QAction::triggered, []() {
        log_to_obs("VortiDeck Overlays (Free) clicked from top-level menu");
        // Open VortiDeck overlays page
        DeepLinkHandler::open_vortideck_with_fallback("overlay");
    });
    
    // Add Connection Settings submenu item  
    QAction* connection_action = vortideck_menu->addAction("Connection Settings");
    QObject::connect(connection_action, &QAction::triggered, []() {
        log_to_obs("VortiDeck Connection Settings clicked from top-level menu");
        if (g_obs_plugin_instance) {
            g_obs_plugin_instance->show_connection_settings_dialog();
        }
    });
    
    log_to_obs("✅ VortiDeck top-level menu created with Banner Settings (ADS), Overlays (Free), and Connection Settings");
}

void vorti::applets::obs_plugin::connection_settings_menu_callback(void* data)
{
    // Static callback for VortiDeck Connection Settings menu
    if (data) {
        plugin_interface* plugin = static_cast<plugin_interface*>(data);
        if (plugin) {
            plugin->show_connection_settings_dialog();
        }
    }
}

void vorti::applets::obs_plugin::add_connection_settings_menu()
{
    // Add VortiDeck Connection Settings menu to OBS
    obs_frontend_add_tools_menu_item("VortiDeck Connection Settings", connection_settings_menu_callback, g_obs_plugin_instance);
    log_to_obs("VortiDeck Connection Settings menu added to OBS Tools menu");
}

void vorti::applets::obs_plugin::show_connection_settings_dialog()
{
    log_to_obs("VortiDeck Connection Settings menu clicked - opening service selection dialog");
    
    // Reset the dialog flag to allow showing the dialog again
    m_show_selection_dialog = false;
    
    // Check if we have any discovered services first
    {
        std::lock_guard<std::mutex> lock(m_discovered_services_mutex);
        if (m_discovered_services.empty()) {
            log_to_obs("No VortiDeck services currently discovered - triggering discovery");
            
            // Set flag to show dialog when discovery completes
            {
                std::lock_guard<std::mutex> dialog_lock(m_dialog_mutex);
                m_pending_dialog_request = true;
            }
            
            // Start discovery if not already running
            if (!m_discovery_in_progress) {
                start_continuous_discovery();
            }
            
            // Show message to user
            log_to_obs("Discovery started - service selection dialog will appear when services are found");
            return;
        }
    }
    
    // Show the service selection dialog with current services
    // This function already handles Qt threading internally
    // Use force_show_dialog=true for manual dialogs to bypass discovery checks
    show_service_selection_dialog(true);
}

// ================================
// Canvas Size Synchronization
// ================================

nlohmann::json vorti::applets::obs_plugin::get_current_canvas_info()
{
    obs_video_info ovi;
    obs_get_video_info(&ovi);
    
    nlohmann::json canvas_info;
    canvas_info["type"] = "OBS_CANVAS_SIZE_UPDATE";
    canvas_info["width"] = ovi.base_width;
    canvas_info["height"] = ovi.base_height;
    canvas_info["fps_num"] = ovi.fps_num;
    canvas_info["fps_den"] = ovi.fps_den;
    canvas_info["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    canvas_info["source"] = "obs_plugin";
    
    return canvas_info;
}

void vorti::applets::obs_plugin::send_canvas_size_update()
{
    try {
        nlohmann::json canvas_info = get_current_canvas_info();
        
        // Check if canvas size actually changed
        CanvasSizeInfo current_size;
        current_size.width = canvas_info["width"];
        current_size.height = canvas_info["height"];
        current_size.fps_num = canvas_info["fps_num"];
        current_size.fps_den = canvas_info["fps_den"];
        
        // Only proceed if size changed
        if (current_size.width == m_last_canvas_size.width &&
            current_size.height == m_last_canvas_size.height &&
            current_size.fps_num == m_last_canvas_size.fps_num &&
            current_size.fps_den == m_last_canvas_size.fps_den) {
            log_to_obs("Canvas size unchanged - skipping update");
            return;
        }
        
        // Update cached size
        m_last_canvas_size = current_size;
        
        if (!is_connected()) {
            log_to_obs(std::format("Canvas size changed to {}x{} @ {}/{} fps - will send when VortiDeck connects", 
                current_size.width, current_size.height, current_size.fps_num, current_size.fps_den));
            return;
        }
        
        // Send the update
        if (send_message(canvas_info)) {
            log_to_obs(std::format("✅ Canvas size update sent: {}x{} @ {}/{} fps", 
                current_size.width, current_size.height, current_size.fps_num, current_size.fps_den));
        } else {
            log_to_obs("❌ Failed to send canvas size update");
        }
        
    } catch (const std::exception& e) {
        log_to_obs(std::format("Error sending canvas size update: {}", e.what()));
    }
}

void vorti::applets::obs_plugin::handle_canvas_size_request(const nlohmann::json& message)
{
    try {
        log_to_obs("Processing canvas size request from VortiDeck");
        
        // Always respond with current canvas info
        nlohmann::json response = get_current_canvas_info();
        
        // Add request timestamp if available
        if (message.contains("timestamp") && !message["timestamp"].is_null()) {
            response["request_timestamp"] = message["timestamp"];
        }
        
        if (send_message(response)) {
            log_to_obs("Canvas size response sent successfully");
        } else {
            log_to_obs("Failed to send canvas size response");
        }
        
    } catch (const std::exception& e) {
        log_to_obs(std::format("Error handling canvas size request: {}", e.what()));
    }
}

// ================================
// Video Reset Signal Handlers
// ================================

void vorti::applets::obs_plugin::handle_video_reset_signal(void *data, calldata_t *cd)
{
    log_to_obs("🎥 Video reset signal detected - sending canvas size update and resizing banner");
    
    // Send canvas size update when video settings reset
    send_canvas_size_update();
    
    // Resize banner to match new canvas size
    try {
        auto& banner_mgr = get_global_banner_manager();
        banner_mgr.resize_banner_to_canvas();
    } catch (...) {
        log_to_obs("Failed to resize banner to canvas - banner_manager not available");
    }
}

void vorti::applets::obs_plugin::connect_video_reset_signals()
{
    signal_handler_t* obs_signals = obs_get_signal_handler();
    if (!obs_signals) {
        log_to_obs("❌ Failed to get OBS signal handler for video reset signals");
        return;
    }
    
    // Connect to video_reset signal (main OBS video reset)
    signal_handler_connect(obs_signals, "video_reset", handle_video_reset_signal, nullptr);
    
    // Connect to canvas_video_reset signal (canvas video reset)  
    signal_handler_connect(obs_signals, "canvas_video_reset", handle_video_reset_signal, nullptr);
    
    log_to_obs("✅ Connected to OBS video reset signals for canvas size sync");
}

void vorti::applets::obs_plugin::disconnect_video_reset_signals()
{
    signal_handler_t* obs_signals = obs_get_signal_handler();
    if (!obs_signals) {
        return;
    }
    
    // Disconnect video reset signals
    signal_handler_disconnect(obs_signals, "video_reset", handle_video_reset_signal, nullptr);
    signal_handler_disconnect(obs_signals, "canvas_video_reset", handle_video_reset_signal, nullptr);
    
    log_to_obs("Disconnected from OBS video reset signals");
}

// ================================
// Deep Link Implementation
// ================================

bool DeepLinkHandler::open_url(const std::string& url)
{
    if (url.empty()) {
        log_to_obs("❌ Deep link error: Empty URL provided");
        return false;
    }
    
    QString qurl = QString::fromStdString(url);
    bool success = QDesktopServices::openUrl(QUrl(qurl));
    
    log_deep_link_result(url, success);
    return success;
}

bool DeepLinkHandler::open_vortideck(const std::string& path)
{
    std::string url = "vortideck://";
    if (!path.empty()) {
        url += "open_page/" + path;
    }
    
    return open_url(url);
}

bool DeepLinkHandler::open_vortideck_with_fallback(const std::string& path)
{
    // First try the deep link
    if (open_vortideck(path)) {
        return true;
    }
    
    // If deep link fails, try opening the web version
    log_to_obs("Deep link failed, trying web fallback...");
    std::string web_url = "https://vortideck.com";
    if (!path.empty()) {
        web_url += "/" + path;
    }
    
    return open_url(web_url);
}

void DeepLinkHandler::log_deep_link_result(const std::string& url, bool success)
{
    std::stringstream ss;
    ss << (success ? "✅" : "❌") << " Deep link " 
       << (success ? "opened successfully" : "failed to open") 
       << ": " << url;
    log_to_obs(ss.str());
}

// Test function implementation
void vorti::applets::obs_plugin::test_open_vortideck_deep_link()
{
    log_to_obs("🧪 Testing VortiDeck deep link...");
    
    // Test opening VortiDeck dashboard
    bool success = DeepLinkHandler::open_vortideck_with_fallback("dashboard");
    
    if (success) {
        log_to_obs("✅ Deep link test completed successfully");
    } else {
        log_to_obs("❌ Deep link test failed");
    }
}
