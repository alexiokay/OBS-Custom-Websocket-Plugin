#include "obs_plugin.hpp"

#define _WEBSOCKETPP_CPP11_RANDOM_DEVICE_

#ifndef ASIO_STANDALONE
    #define ASIO_STANDALONE
#endif

#ifndef _WEBSOCKETPP_CPP11_TYPE_TRAITS_
    #define _WEBSOCKETPP_CPP11_TYPE_TRAITS_
#endif

// WebSocket and JSON includes are already in obs_plugin.hpp
#include <libobs/obs-module.h>

#include <libobs/util/platform.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif
#include <sstream>

using namespace vorti::applets::obs_plugin;

// Simple plugin interface for banner manager
struct plugin_interface {
    bool is_connected() const;
    bool send_message(const nlohmann::json& message) const;
};

// Implementation of plugin_interface methods
bool plugin_interface::is_connected() const {
    return vorti::applets::obs_plugin::is_connected();
}

bool plugin_interface::send_message(const nlohmann::json& message) const {
    return vorti::applets::obs_plugin::send_message(message);
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

// TEMPORARILY COMMENT OUT OBS-SPECIFIC FUNCTIONS
// (Uncommented obs_module_post_load to enable menu functionality)
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
    m_obs_frontend_available = false;
    
    // Start connection in a separate jthread to avoid blocking OBS startup
    std::jthread([]{
        // Add a small delay to let OBS finish initializing
        std::this_thread::sleep_for(std::chrono::seconds(1));
        connect();
    }).detach();

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
        
        // CRITICAL: Clean up banner sources before scene collection changes
        // This prevents "sources could not be unloaded" errors
        if constexpr (BANNER_MANAGER_ENABLED) {
            m_banner_manager.cleanup_for_scene_collection_change();
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
        m_start_time = std::chrono::steady_clock::now();
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
        log_to_obs("DISCONNECT TRIGGER: OBS Frontend Exit Event received");
        
        // Check if shutdown is already in progress to prevent race with module unload
        if (m_shutdown_complete.load()) {
            log_to_obs("DISCONNECT TRIGGER: Shutdown already in progress, skipping frontend exit handling");
            return;
        }
        
        // CRITICAL: Mark frontend as unavailable IMMEDIATELY to prevent API calls
        // Use atomic operations to ensure thread safety
        {
            std::lock_guard<std::mutex> wl(m_lock);
            m_obs_frontend_available = false;
            m_shutting_down = true;
        }
        
        // Notify all threads immediately to stop
        m_compressor_ready_cv.notify_all();
        m_initialization_cv.notify_all();
        
        // Stop banner manager first to prevent OBS API calls during shutdown
        if constexpr (BANNER_MANAGER_ENABLED) {
            m_banner_manager_shutdown.store(true);
            // Don't call full shutdown here as obs_module_unload will handle it
            // Just set the flag to prevent new operations
            log_to_obs("DISCONNECT TRIGGER: Banner manager shutdown flag set");
        }
        
        // Let disconnect() handle the full shutdown sequence with proper guards
        // This will prevent race conditions with obs_module_unload
        stop_loop();
        uninitialize_actions();
        disconnect();
    }
}


void obs_module_post_load()
{
    register_integration();

    // Register all available actions
    initialize_actions();
    register_regular_actions();

    obs_frontend_add_event_callback(handle_obs_frontend_event, nullptr);
    obs_frontend_add_save_callback(handle_obs_frontend_save, nullptr);
    
    // Mark OBS frontend as available after callbacks are registered
    m_obs_frontend_available = true;

    // Register custom VortiDeck Banner source
    if constexpr (BANNER_MANAGER_ENABLED) {
        vorti::applets::banner_manager::register_vortideck_banner_source();
    }

    // Initialize banner functionality
    if constexpr (BANNER_MANAGER_ENABLED) {
        create_obs_menu();
        
        // Enable analytics tracking when plugin starts
        m_banner_manager.enable_analytics_tracking(true, 60); // Report every 60 seconds
        log_to_obs("ANALYTICS: Analytics tracking enabled for VortiDeck ad displays");
    }

    register_actions_broadcast();

    start_loop();
}


void obs_module_unload()
{
    // Check if shutdown is already complete to prevent double unload
    if (vorti::applets::obs_plugin::m_shutdown_complete.load()) {
        blog(LOG_INFO, "[OBS Plugin] Module unload called but shutdown already complete");
        return;
    }

    // Called when the module is unloaded.
    blog(LOG_INFO, "[OBS Plugin] OBS module unloading - setting shutdown flag");
    
    // Set shutdown flag FIRST to stop all threads immediately
    {
        std::lock_guard<std::mutex> wl(vorti::applets::obs_plugin::m_lock);
        vorti::applets::obs_plugin::m_shutting_down = true;
        vorti::applets::obs_plugin::m_obs_frontend_available = false;  // Immediately disable OBS API calls
    }
    
    // Notify all waiting threads to wake up and check shutdown flag
    vorti::applets::obs_plugin::m_compressor_ready_cv.notify_all();
    vorti::applets::obs_plugin::m_initialization_cv.notify_all();

    vorti::applets::obs_plugin::uninitialize_actions();

    // Cleanup banner functionality FIRST (before removing OBS callbacks)
    if constexpr (BANNER_MANAGER_ENABLED) {
        blog(LOG_INFO, "[OBS Plugin] Cleaning up banner manager...");
        // Set shutdown flag to prevent new banner operations
        m_banner_manager_shutdown.store(true);
        
        // Stop and join all banner tracking threads with timeout
        {
            std::lock_guard<std::mutex> lock(m_banner_threads_mutex);
            for (auto& thread : m_banner_tracking_threads) {
                if (thread.joinable()) {
                    thread.request_stop();
                    
                    // Try to join with a timeout
                    auto timeout_start = std::chrono::steady_clock::now();
                    const auto timeout_duration = std::chrono::seconds(2);
                    
                    bool joined = false;
                    while (!joined && (std::chrono::steady_clock::now() - timeout_start) < timeout_duration) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        if (!thread.joinable()) {
                            joined = true;
                            break;
                        }
                    }
                    
                    if (thread.joinable()) {
                        try {
                            thread.join();
                            blog(LOG_INFO, "[OBS Plugin] Banner tracking thread joined successfully");
                        } catch (...) {
                            blog(LOG_WARNING, "[OBS Plugin] Banner tracking thread join failed, detaching");
                            thread.detach();
                        }
                    }
                }
            }
            m_banner_tracking_threads.clear();
        }
        
        // Call explicit cleanup method to stop all threads
        m_banner_manager.shutdown();
        
        // Unregister custom VortiDeck Banner source only if OBS is still available
        try {
            vorti::applets::banner_manager::unregister_vortideck_banner_source();
            blog(LOG_INFO, "[OBS Plugin] VortiDeck Banner source unregistered");
        } catch (...) {
            blog(LOG_WARNING, "[OBS Plugin] Failed to unregister VortiDeck Banner source (OBS may have shut down)");
        }
        
        blog(LOG_INFO, "[OBS Plugin] Banner manager cleanup complete");
    }

    // Remove OBS frontend callbacks - these should be safe to call even during shutdown
    try {
        obs_frontend_remove_event_callback(handle_obs_frontend_event, nullptr);
        obs_frontend_remove_save_callback(handle_obs_frontend_save, nullptr);
        blog(LOG_INFO, "[OBS Plugin] OBS frontend callbacks removed");
    } catch (...) {
        blog(LOG_WARNING, "[OBS Plugin] Failed to remove OBS frontend callbacks (OBS may have shut down)");
    }

    // Final websocket disconnect
    vorti::applets::obs_plugin::disconnect();
    
    blog(LOG_INFO, "[OBS Plugin] OBS module unload complete");
}

bool vorti::applets::obs_plugin::is_obs_frontend_available()
{
    return !m_shutting_down.load() && m_obs_frontend_available.load();
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

    // Wait for connection with timeout
    {
        std::unique_lock<std::mutex> lk(m_compressor_ready_mutex);
        if (m_compressor_ready_cv.wait_until(lk, std::chrono::system_clock::now() + std::chrono::seconds(10)) == std::cv_status::timeout) {
            log_to_obs("DISCONNECT TRIGGER: Connection timeout after 10 seconds - cleaning up");
            disconnect();
            return false;
        }
    }

    // Start initialization sequence
    log_to_obs("Connection established, starting initialization sequence...");

    // Register integration first
    if (!register_integration()) {
        log_to_obs("Failed to register integration");
        disconnect();
        return false;
    }
    log_to_obs("Integration registered");

    // Initialize actions
    if (!initialize_actions()) {
        log_to_obs("Failed to initialize actions");
        disconnect();
        return false;
    }
    log_to_obs("Actions initialized");

    // Wait for initialization to complete
    {
        std::unique_lock<std::mutex> lk(m_initialization_mutex);
        if (m_initialization_cv.wait_until(lk, std::chrono::system_clock::now() + std::chrono::seconds(15)) == std::cv_status::timeout) {
            log_to_obs("DISCONNECT TRIGGER: Initialization timeout after 15 seconds");
            disconnect();
            return false;
        }
        
        // Check if initialization was actually successful
        if (m_integration_instance.empty() || m_integration_guid.empty()) {
            log_to_obs("DISCONNECT TRIGGER: Initialization failed - missing integration details");
            disconnect();
            return false;
        }
    }

    // Register regular actions
    log_to_obs("Registering regular actions...");
    register_regular_actions();

    // Register parameterized actions
    if (helper_populate_collections()) {
        log_to_obs("Registering parameterized actions...");
        register_parameter_actions();
    }

    // Register action broadcast subscription
    if (!register_actions_broadcast()) {
        log_to_obs("Failed to register action broadcast");
        disconnect();
        return false;
    }

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


void vorti::applets::obs_plugin::websocket_close_handler(const websocketpp::connection_hdl &connection_handle [[maybe_unused]])
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
    // Prevent multiple shutdowns using atomic compare-and-swap
    bool expected = false;
    if (!m_shutdown_complete.compare_exchange_strong(expected, true)) {
        log_to_obs("Disconnect: Shutdown already in progress or complete, skipping");
        return;
    }

    // Add detailed logging to identify who triggered the disconnect
    log_to_obs("Disconnect: Starting shutdown sequence - thread ID: " + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    
    // Set connection state first and mark frontend as unavailable
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
        m_integration_guid.clear();
        m_integration_instance.clear();
        m_current_message_id = 1;
        m_shutting_down = true;  // Ensure shutdown flag is set
        m_obs_frontend_available = false;  // Prevent OBS API calls
    }

    // Notify all waiting threads immediately
    m_compressor_ready_cv.notify_all();
    m_initialization_cv.notify_all();

    // Stop the status update loop first
    stop_loop();

    // CRITICAL: Improved websocket shutdown with proper ASIO cleanup
    if (m_websocket_thread)
    {
        try {
            log_to_obs("Disconnect: Stopping websocket and ASIO threads");
            
            // STEP 1: Stop the websocket event loop - this should interrupt m_websocket.run()
            m_websocket.stop();
            
            // STEP 2: Additional stop operations (websocketpp doesn't expose io_context directly)
            // The stop() call above should be sufficient to interrupt the event loop
            
            // STEP 3: Wait for jthread to finish with timeout to prevent hanging
            if (m_websocket_thread->joinable())
            {
                log_to_obs("Disconnect: Requesting thread stop");
                m_websocket_thread->request_stop();
                
                // Wait up to 5 seconds for graceful shutdown (increased from 3)
                auto timeout_start = std::chrono::steady_clock::now();
                const auto timeout_duration = std::chrono::seconds(5);
                
                bool thread_finished [[maybe_unused]] = false;
                while ((std::chrono::steady_clock::now() - timeout_start) < timeout_duration) {
                    if (!m_websocket_thread->joinable()) {
                        thread_finished = true;
                        break;
                    }
                    
                    // Try to join with a short timeout
                    try {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        // Check if thread finished
                        if (m_websocket_thread->get_stop_token().stop_requested()) {
                            // Give it a bit more time to clean up
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            break;
                        }
                    } catch (...) {
                        break;
                    }
                }
                
                // STEP 4: Final join attempt
                if (m_websocket_thread->joinable()) {
                    try {
                        // One last attempt to join
                        m_websocket_thread->join();
                        log_to_obs("Disconnect: Thread joined successfully");
                    } catch (const std::exception& e) {
                        log_to_obs("Disconnect: Thread join failed: " + std::string(e.what()));
                        // Thread is truly stuck - this is a last resort
                        log_to_obs("Disconnect: WARNING - Thread appears stuck, forcing cleanup");
                    }
                }
            }
            
            // STEP 5: Reset the websocket client state - do this even if thread is stuck
            // The websocket destructor should clean up any remaining ASIO state
            try {
                m_websocket.reset();
                log_to_obs("Disconnect: WebSocket client state reset");
            } catch (const std::exception& e) {
                log_to_obs("Disconnect: Error resetting websocket: " + std::string(e.what()));
            }
            
        } catch (const std::exception& e) {
            log_to_obs("Error during WebSocket shutdown: " + std::string(e.what()));
        }
        
        // Reset the thread - at this point we've done everything possible
        m_websocket_thread.reset();
        log_to_obs("Disconnect: Websocket thread cleanup complete");
    }

    log_to_obs("Disconnect: Shutdown sequence complete");
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
                log_to_obs("Failed to create connection");
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
            
            // Reset for next attempt
            m_websocket.reset();
            
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
            m_websocket.reset();
            
            // Interruptible sleep - check shutdown every 100ms
            for (int i = 0; i < 10 && !m_shutting_down; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}


ws_client::connection_ptr vorti::applets::obs_plugin::_create_connection()
{
    websocketpp::lib::error_code error_code;
            auto connection = m_websocket.get_connection(std::format("ws://192.168.178.129:{}", m_current_port), error_code);

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


void vorti::applets::obs_plugin::websocket_open_handler(const websocketpp::connection_hdl &connection_handle [[maybe_unused]])
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
}


void vorti::applets::obs_plugin::websocket_message_handler(const websocketpp::connection_hdl &connection_handle [[maybe_unused]],
                                                          const ws_client::message_ptr &response)
{
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
                }

                m_initialization_cv.notify_all();
            }
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
        else if (action_id == actions::s_banner_set_data)
        {
            action_banner_set_data(parameters);
        }
        // Essential banner queue and analytics actions
        else if (action_id == actions::s_banner_set_queue)
        {
            action_banner_set_queue(parameters);
        }
        else if (action_id == actions::s_banner_add_to_queue)
        {
            action_banner_add_to_queue(parameters);
        }
        else if (action_id == actions::s_analytics_get_report)
        {
            action_analytics_get_report(parameters);
        }
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


void vorti::applets::obs_plugin::websocket_fail_handler(const websocketpp::connection_hdl &connection_handle [[maybe_unused]])
{
    /* Connection failed - let automatic reconnection handle recovery */
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
    }
    
    log_to_obs("WebSocket connection failed - automatic reconnection will retry");
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

    // ADD THESE BANNER ACTIONS:
    if constexpr (BANNER_MANAGER_ENABLED) {
        actions.push_back(register_action(actions::s_banner_show, "APPLET_OBS_BANNER_SHOW"));
        actions.push_back(register_action(actions::s_banner_hide, "APPLET_OBS_BANNER_HIDE"));
        actions.push_back(register_action(actions::s_banner_toggle, "APPLET_OBS_BANNER_TOGGLE"));
        
        // Banner set data action with enhanced parameters (CSS, dimensions)
        action_parameters banner_params;
        nlohmann::json content_data_param = {
            {"name", "content_data"},
            {"displayName", "Content Data"},
            {"description", "Image URL, Base64 data, or file path"}
        };
        nlohmann::json content_type_param = {
            {"name", "content_type"},
            {"displayName", "Content Type"},
            {"description", "MIME type (image/png, image/jpeg, video/mp4, text, color, etc.)"}
        };
        nlohmann::json css_param = {
            {"name", "css"},
            {"displayName", "Custom CSS"},
            {"description", "Optional custom CSS styling (overrides default styles)"}
        };
        nlohmann::json width_param = {
            {"name", "width"},
            {"displayName", "Width"},
            {"description", "Optional custom width in pixels (default: 1920)"}
        };
        nlohmann::json height_param = {
            {"name", "height"},
            {"displayName", "Height"},
            {"description", "Optional custom height in pixels (default: auto-sized by content type)"}
        };
        banner_params.push_back(content_data_param);
        banner_params.push_back(content_type_param);
        banner_params.push_back(css_param);
        banner_params.push_back(width_param);
        banner_params.push_back(height_param);
        actions.push_back(register_action(actions::s_banner_set_data, "APPLET_OBS_BANNER_SET_DATA", banner_params));
    }

    if constexpr (BANNER_MANAGER_ENABLED) {
        actions.push_back(register_action(actions::s_banner_show, "APPLET_OBS_BANNER_SHOW"));
        actions.push_back(register_action(actions::s_banner_hide, "APPLET_OBS_BANNER_HIDE"));
        actions.push_back(register_action(actions::s_banner_toggle, "APPLET_OBS_BANNER_TOGGLE"));
    }

    if constexpr (BANNER_MANAGER_ENABLED) {
        actions.push_back(register_action(actions::s_banner_show, "APPLET_OBS_BANNER_SHOW"));
        actions.push_back(register_action(actions::s_banner_hide, "APPLET_OBS_BANNER_HIDE"));
        actions.push_back(register_action(actions::s_banner_toggle, "APPLET_OBS_BANNER_TOGGLE"));
        
        // Essential banner queue and analytics actions
        actions.push_back(register_action(actions::s_banner_set_queue, "APPLET_OBS_BANNER_SET_QUEUE"));
        actions.push_back(register_action(actions::s_banner_add_to_queue, "APPLET_OBS_BANNER_ADD_TO_QUEUE"));
        actions.push_back(register_action(actions::s_analytics_get_report, "APPLET_OBS_ANALYTICS_GET_REPORT"));
    }

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

    if (!is_obs_frontend_available()) {
        log_to_obs("Cannot start streaming - OBS frontend unavailable");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("Cannot stop streaming - OBS frontend unavailable");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("Cannot toggle streaming - OBS frontend unavailable");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("ACTION_RECORDING_START: OBS frontend not available (shutdown in progress)");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("ACTION_RECORDING_STOP: OBS frontend not available (shutdown in progress)");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("ACTION_RECORDING_TOGGLE: OBS frontend not available (shutdown in progress)");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("ACTION_BUFFER_START: OBS frontend not available (shutdown in progress)");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("ACTION_BUFFER_STOP: OBS frontend not available (shutdown in progress)");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("ACTION_BUFFER_TOGGLE: OBS frontend not available (shutdown in progress)");
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

    if (!is_obs_frontend_available()) {
        log_to_obs("ACTION_BUFFER_SAVE: OBS frontend not available (shutdown in progress)");
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

        auto sourceEnumProc = [](obs_scene_t *scene [[maybe_unused]], obs_sceneitem_t *currentItem, void *privateData) -> bool {
            auto *parameters = (new_state_info *)privateData;
            obs_source_t *source = obs_sceneitem_get_source(currentItem);
            uint32_t source_type [[maybe_unused]] = obs_source_get_output_flags(source);
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

        auto sourceEnumProc = [](obs_scene_t *scene [[maybe_unused]], obs_sceneitem_t *currentItem, void *privateData) -> bool {
            auto *parameters = (new_state_info *)privateData;
            obs_source_t *source = obs_sceneitem_get_source(currentItem);
            uint32_t source_type [[maybe_unused]] = obs_source_get_output_flags(source);
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
        for (const auto &source : scene.second.source_list)
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
        for (const auto &source : scene.second.mixer_list)
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

        current_collection_scene.source_list.clear();

        auto sourceEnumProc = [](obs_scene_t *scene [[maybe_unused]], obs_sceneitem_t *currentItem, void *privateData) -> bool {
            auto *parameters = (scene_info *)privateData;

            obs_source_t *source = obs_sceneitem_get_source(currentItem);
            uint32_t source_type [[maybe_unused]] = obs_source_get_output_flags(source);

            if (((source_type & OBS_SOURCE_VIDEO) == OBS_SOURCE_VIDEO)
                || ((source_type & OBS_SOURCE_ASYNC) == OBS_SOURCE_ASYNC))
            {
                parameters->source_list.push_back(std::string(obs_source_get_name(source)));
            }
            else if ((source_type & OBS_SOURCE_AUDIO) == OBS_SOURCE_AUDIO)
            {
                parameters->mixer_list.push_back(std::string(obs_source_get_name(source)));
            }

            return true;
        };
        obs_scene_enum_items(scene, sourceEnumProc, obs_scenes);

        for (const auto &scene_element : obs_scenes->source_list)
        {
            current_collection_scene.source_list.push_back(scene_element);
        }

        for (const auto &scene_element : obs_scenes->mixer_list)
        {
            current_collection_scene.mixer_list.push_back(scene_element);
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

        // Safety check: Don't call OBS APIs if frontend is unavailable
        if (!is_obs_frontend_available()) {
            status_payload["currentState"] = "IDLE";
        }
        else if (obs_frontend_streaming_active())
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
            auto duration = std::chrono::steady_clock::now() - m_start_time;
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

        if (is_obs_frontend_available()) {
            char *current_profile = obs_frontend_get_current_profile();
            if (current_profile != nullptr)
            {
                status_payload["activeProfile"] = std::string(current_profile);
                bfree(current_profile);
            }
        }

        if (is_obs_frontend_available()) {
            char *current_collection = obs_frontend_get_current_scene_collection();
            if (current_collection != nullptr)
            {
                status_payload["activeCollection"] = std::string(current_collection);
                bfree(current_collection);
            }
        }

        if (is_obs_frontend_available()) {
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
            
            m_banner_manager.track_viewer_metrics(estimated_viewers, bps, fps);
            
            // Send periodic batch analytics reports
            m_banner_manager.send_batch_analytics_report();
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
}

// Menu callbacks are now handled by the banner manager

// Banner action implementations
void vorti::applets::obs_plugin::action_banner_show(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        if (m_banner_manager_shutdown.load()) {
            log_to_obs("ACTION_BANNER_SHOW: Rejected - shutdown in progress");
            return;
        }
        
        log_to_obs("ACTION_BANNER_SHOW: Command received from API");
        
        // Check premium status and schedule VortiDeck ads
        handle_banner_premium_status_and_ads(parameters);
        
        log_to_obs("ACTION_BANNER_SHOW: Calling banner manager show_banner()");
        m_banner_manager.show_banner();
        
        // Log creator banner activity with revenue share
        float revenue_share = m_banner_manager.get_revenue_share();
        std::string user_type = m_banner_manager.is_premium_user() ? "premium" : "free";
        log_to_obs("ACTION_BANNER_SHOW: Banner show completed - " + user_type + " user (" + 
                   std::to_string(revenue_share * 100) + "% revenue share)");
    } else {
        log_to_obs("ACTION_BANNER_SHOW: Banner manager disabled");
    }
}

void vorti::applets::obs_plugin::action_banner_hide(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        if (m_banner_manager_shutdown.load()) {
            log_to_obs("ACTION_BANNER_HIDE: Rejected - shutdown in progress");
            return;
        }
        
        log_to_obs("ACTION_BANNER_HIDE: Command received from API");
        
        // Check premium status and schedule VortiDeck ads
        handle_banner_premium_status_and_ads(parameters);
        
        // Additional free user warning
        if (!m_banner_manager.is_premium_user()) {
            log_to_obs("ACTION_BANNER_HIDE: FREE USER - Banner hiding very limited (5sec auto-restore) - upgrade to premium for full control");
        }
        
        log_to_obs("ACTION_BANNER_HIDE: Calling banner manager hide_banner()");
        m_banner_manager.hide_banner();
        
        log_to_obs("ACTION_BANNER_HIDE: Banner hide completed");
        
        // Safety check: Ensure protection system will re-enable for free users
        if (!m_banner_manager.is_premium_user()) {
            log_to_obs("ACTION_BANNER_HIDE: FREE USER - Protection system will auto-restore in 5-10 seconds");
        }
    } else {
        log_to_obs("ACTION_BANNER_HIDE: Banner manager disabled");
    }
}

void vorti::applets::obs_plugin::action_banner_toggle(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        if (m_banner_manager_shutdown.load()) {
            log_to_obs("ACTION_BANNER_TOGGLE: Rejected - shutdown in progress");
            return;
        }
        
        log_to_obs("ACTION_BANNER_TOGGLE: Command received from API");
        
        // Check premium status and schedule VortiDeck ads
        handle_banner_premium_status_and_ads(parameters);
        
        // Additional free user warning for toggle
        if (!m_banner_manager.is_premium_user()) {
            log_to_obs("ACTION_BANNER_TOGGLE: FREE USER - Banner toggle very limited (hiding auto-restores in 5sec) - upgrade to premium for full control");
        }
        
        log_to_obs("ACTION_BANNER_TOGGLE: Calling banner manager toggle_banner()");
        m_banner_manager.toggle_banner();
        
        log_to_obs("ACTION_BANNER_TOGGLE: Banner toggle completed");
        
        // Safety check: Ensure protection system is active for free users
        if (!m_banner_manager.is_premium_user()) {
            // Give a moment for the toggle to complete, then ensure protection is active
            {
                std::lock_guard<std::mutex> lock(m_banner_threads_mutex);
                m_banner_tracking_threads.emplace_back([]() {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (!m_banner_manager_shutdown.load()) {
                        log_to_obs("ACTION_BANNER_TOGGLE: FREE USER - Ensuring protection system is active");
                        // Note: The banner manager handles its own protection re-enabling
                    }
                });
            }
        }
    } else {
        log_to_obs("ACTION_BANNER_TOGGLE: Banner manager disabled");
    }
}

void vorti::applets::obs_plugin::action_banner_set_data(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        if (m_banner_manager_shutdown.load()) {
            log_to_obs("ACTION_BANNER_SET_DATA: Rejected - shutdown in progress");
            return;
        }
        
        log_to_obs("ACTION_BANNER_SET_DATA: Command received from API");
        
        // Check premium status and schedule VortiDeck ads
        handle_banner_premium_status_and_ads(parameters);
        
        auto content_data_it = parameters.find("content_data");
        auto content_type_it = parameters.find("content_type");
        
        // Check for queue_mode parameter
        bool queue_mode = false;
        auto queue_mode_it = parameters.find("queue_mode");
        if (queue_mode_it != parameters.end()) {
            queue_mode = (queue_mode_it->second == "true");
        }
        
        // Check for auto_rotate parameter (only applies to queue mode)
        bool auto_rotate = true; // Default to true for queue mode
        auto auto_rotate_it = parameters.find("auto_rotate");
        if (auto_rotate_it != parameters.end()) {
            auto_rotate = (auto_rotate_it->second == "true");
        }
        
        if (content_data_it != parameters.end() && content_type_it != parameters.end()) {
            // Extract analytics parameters
            std::string ad_id = "ad_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            std::string campaign_id = "default_campaign";
            int planned_duration_ms = 30000; // Default 30 seconds
            
            // Check for ad_id parameter
            auto ad_id_it = parameters.find("ad_id");
            if (ad_id_it != parameters.end()) {
                ad_id = ad_id_it->second;
            }
            
            // Check for campaign_id parameter
            auto campaign_id_it = parameters.find("campaign_id");
            if (campaign_id_it != parameters.end()) {
                campaign_id = campaign_id_it->second;
            }
            
            // Check for duration parameter
            auto duration_it = parameters.find("duration_seconds");
            if (duration_it != parameters.end()) {
                try {
                    planned_duration_ms = std::stoi(duration_it->second) * 1000;
                } catch (const std::exception&) {
                    log_to_obs("ACTION_BANNER_SET_DATA: Invalid duration format, using default 30 seconds");
                }
            }
            
            log_to_obs("ACTION_BANNER_SET_DATA: Setting banner content - Type: " + content_type_it->second + 
                      ", Data: " + content_data_it->second.substr(0, 50) + "..." +
                      ", Ad ID: " + ad_id + ", Campaign: " + campaign_id + 
                      ", Queue Mode: " + (queue_mode ? "true" : "false") +
                      (queue_mode ? ", Auto-Rotate: " + std::string(auto_rotate ? "true" : "false") : ""));
            
            // Start analytics tracking (only for immediate mode - queue mode handles its own analytics)
            if constexpr (BANNER_MANAGER_ENABLED) {
                if (!queue_mode) {
                    m_banner_manager.track_ad_display_start(ad_id, campaign_id, planned_duration_ms, content_type_it->second);
                }
            }
            
            // Check for optional CSS parameter
            auto css_it = parameters.find("css");
            std::string custom_css = "";
            if (css_it != parameters.end()) {
                custom_css = css_it->second;
                log_to_obs("ACTION_BANNER_SET_DATA: Custom CSS provided: " + custom_css.substr(0, 100) + "...");
            }
            
            // Check for optional width/height parameters
            auto width_it = parameters.find("width");
            auto height_it = parameters.find("height");
            int custom_width = 0;
            int custom_height = 0;
            
            // Note: animation_duration and auto_hide parameters are passed through but not processed 
            // Animations are handled purely in CSS
            // Auto-hide is handled automatically by queue/window management
            
            if (width_it != parameters.end()) {
                try {
                    custom_width = std::stoi(width_it->second);
                    log_to_obs("ACTION_BANNER_SET_DATA: Custom width: " + std::to_string(custom_width));
                } catch (const std::invalid_argument& e) {
                    log_to_obs("ACTION_BANNER_SET_DATA: Invalid width format: " + width_it->second + " (" + e.what() + ")");
                } catch (const std::out_of_range& e) {
                    log_to_obs("ACTION_BANNER_SET_DATA: Width value out of range: " + width_it->second + " (" + e.what() + ")");
                }
            }
            
            if (height_it != parameters.end()) {
                try {
                    custom_height = std::stoi(height_it->second);
                    log_to_obs("ACTION_BANNER_SET_DATA: Custom height: " + std::to_string(custom_height));
                } catch (const std::invalid_argument& e) {
                    log_to_obs("ACTION_BANNER_SET_DATA: Invalid height format: " + height_it->second + " (" + e.what() + ")");
                } catch (const std::out_of_range& e) {
                    log_to_obs("ACTION_BANNER_SET_DATA: Height value out of range: " + height_it->second + " (" + e.what() + ")");
                }
            }
            
            // Handle queue mode vs immediate display
            if (queue_mode) {
                // QUEUE MODE: Add banner to queue instead of immediate display
                log_to_obs("ACTION_BANNER_SET_DATA: Queue mode - adding banner to queue");
                
                // Create AdWindow object
                banner_manager::AdWindow ad_window;
                ad_window.id = ad_id;
                ad_window.content_data = content_data_it->second;
                ad_window.content_type = content_type_it->second;
                ad_window.css = custom_css;
                ad_window.duration_seconds = planned_duration_ms / 1000; // Convert to seconds
                ad_window.auto_rotate = auto_rotate;
                ad_window.start_time = std::chrono::system_clock::now();
                
                // Enable auto-rotation if requested (only for queue mode)
                if (auto_rotate && !m_banner_manager.is_auto_rotation_enabled()) {
                    m_banner_manager.enable_auto_rotation(true);
                    log_to_obs("ACTION_BANNER_SET_DATA: Auto-rotation enabled for queue mode");
                } else if (!auto_rotate && m_banner_manager.is_auto_rotation_enabled()) {
                    m_banner_manager.enable_auto_rotation(false);
                    log_to_obs("ACTION_BANNER_SET_DATA: Auto-rotation disabled for manual control");
                }
                
                // Add to queue
                m_banner_manager.add_ad_to_queue(ad_window);
                
                log_to_obs("ACTION_BANNER_SET_DATA: Banner '" + ad_id + "' added to queue successfully");
                
            } else {
                // IMMEDIATE MODE: Current replacement behavior
                log_to_obs("ACTION_BANNER_SET_DATA: Immediate mode - replacing current banner");
                
                // Check ad frequency limits for free users BEFORE showing banner
                if (!m_banner_manager.is_premium_user() && !m_banner_manager.can_show_ad_now()) {
                    log_to_obs("ACTION_BANNER_SET_DATA: FREE USER - Banner blocked due to ad frequency limit (1 minute between ads)");
                    log_to_obs("ACTION_BANNER_SET_DATA: Upgrade to premium for unlimited ad frequency");
                    return; // Exit early without showing banner
                }
                
                // Set custom duration if provided
                int custom_duration_seconds = planned_duration_ms / 1000;
                if (custom_duration_seconds > 0) {
                    m_banner_manager.set_custom_banner_duration(custom_duration_seconds);
                    log_to_obs("ACTION_BANNER_SET_DATA: Set custom duration: " + std::to_string(custom_duration_seconds) + " seconds");
                }
                
                // Set banner content with custom parameters
                m_banner_manager.set_banner_content_with_custom_params(
                    content_data_it->second, 
                    content_type_it->second,
                    custom_css,
                    custom_width,
                    custom_height
                );
                
                // Show banner using appropriate method based on user type
                if (m_banner_manager.is_premium_user()) {
                    log_to_obs("ACTION_BANNER_SET_DATA: Premium user - showing banner with freedom");
                    m_banner_manager.show_premium_banner();
                } else {
                    log_to_obs("ACTION_BANNER_SET_DATA: Free user - showing banner with restrictions");
                    m_banner_manager.show_banner();
                }
            }
            
            // Log creator banner activity with revenue share
            float revenue_share = m_banner_manager.get_revenue_share();
            std::string user_type = m_banner_manager.is_premium_user() ? "premium" : "free";
            std::string mode_desc = queue_mode ? "queued" : "displayed immediately";
            log_to_obs("ACTION_BANNER_SET_DATA: Banner " + mode_desc + " successfully - " + user_type + " user (" + 
                       std::to_string(revenue_share * 100) + "% revenue share)");
            
            // Schedule analytics end tracking (only for immediate mode - queue mode handles its own analytics)
            if constexpr (BANNER_MANAGER_ENABLED) {
                if (!queue_mode) {
                    // Use managed thread instead of detached to prevent use-after-free
                    std::lock_guard<std::mutex> lock(m_banner_threads_mutex);
                    m_banner_tracking_threads.emplace_back([ad_id, planned_duration_ms]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(planned_duration_ms));
                        // Check if banner manager is still valid before calling
                        if (!m_banner_manager_shutdown.load()) {
                            m_banner_manager.track_ad_display_end(ad_id, planned_duration_ms);
                        }
                    });
                }
            }
        } else {
            log_to_obs("ACTION_BANNER_SET_DATA: ERROR - Missing required parameters (content_data and content_type)");
        }
    } else {
        log_to_obs("ACTION_BANNER_SET_DATA: Banner manager disabled");
    }
}



// Premium status and monetization helper
void vorti::applets::obs_plugin::handle_banner_premium_status_and_ads(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        log_to_obs("PREMIUM_STATUS: Processing premium status and ad scheduling");
        
        // Convert parameters to JSON for premium status checking
        nlohmann::json premium_data;
        
        // Check for premium status in parameters
        auto premium_it = parameters.find("premium_status");
        if (premium_it != parameters.end()) {
            premium_data["premium_status"] = (premium_it->second == std::string("true"));
            log_to_obs("PREMIUM_STATUS: Premium status parameter found: " + premium_it->second);
        } else {
            log_to_obs("PREMIUM_STATUS: No premium status parameter - using default (free)");
        }
        
        // Check for custom ad frequency (premium users only)
        auto ad_freq_it = parameters.find("ad_frequency_minutes");
        if (ad_freq_it != parameters.end()) {
            try {
                int frequency = std::stoi(ad_freq_it->second);
                premium_data["ad_frequency_minutes"] = frequency;
                log_to_obs("PREMIUM_STATUS: Ad frequency parameter found: " + std::to_string(frequency) + " minutes");
            } catch (const std::invalid_argument& e) {
                log_to_obs("PREMIUM_STATUS: Invalid ad_frequency_minutes format: " + ad_freq_it->second + " (" + e.what() + ")");
            } catch (const std::out_of_range& e) {
                log_to_obs("PREMIUM_STATUS: Ad frequency value out of range: " + ad_freq_it->second + " (" + e.what() + ")");
            }
        }
        
        // Update premium status in banner manager
        log_to_obs("PREMIUM_STATUS: Updating banner manager premium status");
        m_banner_manager.update_premium_status(premium_data);
        
        log_to_obs("PREMIUM_STATUS: Premium status update completed");
    } else {
        log_to_obs("PREMIUM_STATUS: Banner manager disabled");
    }
}
// Banner helper implementations are now in obs_plugin_banner.cpp
// These helper functions are kept for backward compatibility but delegate to the actual implementations

// ============================================================================
// BANNER QUEUE AND ANALYTICS ACTION IMPLEMENTATIONS  
// ============================================================================

void vorti::applets::obs_plugin::action_banner_set_queue(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        log_to_obs("ACTION_BANNER_SET_QUEUE: Setting banner queue");
        
        // Parse banners array from parameters (expecting JSON string)
        auto banners_it = parameters.find("banners");
        if (banners_it == parameters.end()) {
            log_to_obs("ACTION_BANNER_SET_QUEUE: ERROR - Missing 'banners' parameter");
            return;
        }
        
        try {
            // Parse JSON banners array
            nlohmann::json banners_json = nlohmann::json::parse(banners_it->second);
            
            if (!banners_json.is_array()) {
                log_to_obs("ACTION_BANNER_SET_QUEUE: ERROR - 'banners' must be an array");
                return;
            }
            
            // Convert JSON to AdWindow objects
            std::vector<banner_manager::AdWindow> queue;
            
            for (const auto& banner : banners_json) {
                if (!banner.contains("id") || !banner.contains("content_data") || !banner.contains("content_type")) {
                    log_to_obs("ACTION_BANNER_SET_QUEUE: Skipping banner - missing required fields");
                    continue;
                }
                
                banner_manager::AdWindow ad_window;
                ad_window.id = banner["id"].get<std::string>();
                ad_window.content_data = banner["content_data"].get<std::string>();
                ad_window.content_type = banner["content_type"].get<std::string>();
                ad_window.duration_seconds = banner.value("duration_seconds", 30);
                ad_window.css = banner.value("css", "");
                ad_window.auto_rotate = banner.value("auto_rotate", true);
                ad_window.start_time = std::chrono::system_clock::now();
                
                queue.push_back(ad_window);
                
                log_to_obs("ACTION_BANNER_SET_QUEUE: Added banner '" + ad_window.id + "' (" + 
                          std::to_string(ad_window.duration_seconds) + "s)");
            }
            
            if (queue.empty()) {
                log_to_obs("ACTION_BANNER_SET_QUEUE: WARNING - No valid banners to add to queue");
                return;
            }
            
            // Handle premium status and monetization
            handle_banner_premium_status_and_ads(parameters);
            
            // Set the queue in banner manager
            m_banner_manager.set_ad_queue(queue);
            
            log_to_obs("ACTION_BANNER_SET_QUEUE: Successfully set queue with " + 
                      std::to_string(queue.size()) + " banners");
            
        } catch (const nlohmann::json::exception& e) {
            log_to_obs("ACTION_BANNER_SET_QUEUE: JSON parsing error: " + std::string(e.what()));
        } catch (const std::exception& e) {
            log_to_obs("ACTION_BANNER_SET_QUEUE: Error: " + std::string(e.what()));
        }
        
    } else {
        log_to_obs("ACTION_BANNER_SET_QUEUE: Banner manager disabled");
    }
}

void vorti::applets::obs_plugin::action_banner_add_to_queue(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        log_to_obs("ACTION_BANNER_ADD_TO_QUEUE: Adding banner to queue");
        
        // Get required parameters
        auto id_it = parameters.find("id");
        auto content_data_it = parameters.find("content_data");
        auto content_type_it = parameters.find("content_type");
        
        if (id_it == parameters.end() || content_data_it == parameters.end() || content_type_it == parameters.end()) {
            log_to_obs("ACTION_BANNER_ADD_TO_QUEUE: ERROR - Missing required parameters (id, content_data, content_type)");
            return;
        }
        
        // Create AdWindow object
        banner_manager::AdWindow ad_window;
        ad_window.id = id_it->second;
        ad_window.content_data = content_data_it->second;
        ad_window.content_type = content_type_it->second;
        ad_window.start_time = std::chrono::system_clock::now();
        
        // Get optional parameters
        auto duration_it = parameters.find("duration_seconds");
        if (duration_it != parameters.end()) {
            try {
                ad_window.duration_seconds = std::stoi(duration_it->second);
            } catch (const std::exception&) {
                ad_window.duration_seconds = 30; // Default
            }
        } else {
            ad_window.duration_seconds = 30; // Default
        }
        
        auto css_it = parameters.find("css");
        if (css_it != parameters.end()) {
            ad_window.css = css_it->second;
        }
        
        auto auto_rotate_it = parameters.find("auto_rotate");
        if (auto_rotate_it != parameters.end()) {
            ad_window.auto_rotate = (auto_rotate_it->second == "true");
        } else {
            ad_window.auto_rotate = true; // Default
        }
        
        // Handle premium status and monetization
        handle_banner_premium_status_and_ads(parameters);
        
        // Add to queue
        m_banner_manager.add_ad_to_queue(ad_window);
        
        log_to_obs("ACTION_BANNER_ADD_TO_QUEUE: Successfully added banner '" + ad_window.id + 
                  "' (" + std::to_string(ad_window.duration_seconds) + "s) to queue");
        
    } else {
        log_to_obs("ACTION_BANNER_ADD_TO_QUEUE: Banner manager disabled");
    }
}

void vorti::applets::obs_plugin::action_analytics_get_report(const action_invoke_parameters &parameters [[maybe_unused]])
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        log_to_obs("ACTION_ANALYTICS_GET_REPORT: Generating analytics report");
        
        // Force send a batch analytics report
        m_banner_manager.send_batch_analytics_report();
        
        // Send queue status
        if (is_connected()) {
            // Get current queue status
            nlohmann::json queue_status = {
                {"path", "/api/v1/obs/analytics/queue-status"},
                {"verb", "SET"},
                {"payload", {
                    {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()},
                    {"current_viewer_count", m_banner_manager.calculate_current_viewer_estimate()},
                    {"premium_user", m_banner_manager.is_premium_user()},
                    {"revenue_share", m_banner_manager.get_revenue_share()},
                    {"plugin_version", "1.0.0"},
                    {"obs_version", obs_get_version_string()},
                    {"system_status", "operational"}
                }}
            };
            
            if (send_message(queue_status)) {
                log_to_obs("ACTION_ANALYTICS_GET_REPORT: Queue status sent successfully");
            } else {
                log_to_obs("ACTION_ANALYTICS_GET_REPORT: Failed to send queue status");
            }
        }
        
        log_to_obs("ACTION_ANALYTICS_GET_REPORT: Analytics report sent to VortiDeck");
        
    } else {
        log_to_obs("ACTION_ANALYTICS_GET_REPORT: Banner manager disabled");
    }
}




