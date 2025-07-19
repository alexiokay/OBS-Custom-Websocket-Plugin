#include "obs_plugin.hpp"
#include "service_selection_dialog.hpp"
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
        m_shutting_down = true;
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
    
    // Start connection in a separate jthread to avoid blocking OBS startup
    std::jthread([]{
        // Add a small delay to let OBS finish initializing
        std::this_thread::sleep_for(std::chrono::seconds(1));
        connect();
    }).detach();
    register_regular_actions();

    obs_frontend_add_event_callback(handle_obs_frontend_event, nullptr);
    obs_frontend_add_save_callback(handle_obs_frontend_save, nullptr);

    // Register custom VortiDeck Banner source
    if constexpr (BANNER_MANAGER_ENABLED) {
        vorti::applets::banner_manager::register_vortideck_banner_source();
    }

    // Initialize banner functionality
    if constexpr (BANNER_MANAGER_ENABLED) {
        create_obs_menu();
        
        // Analytics tracking removed - simplified banner system
    }

    register_actions_broadcast();

    start_loop();
}

void obs_module_unload()
{
    // Called when the module is unloaded.
    blog(LOG_INFO, "[OBS Plugin] OBS module unloading - setting shutdown flag");
    
    // Set shutdown flag FIRST to stop all threads immediately
    {
        std::lock_guard<std::mutex> wl(vorti::applets::obs_plugin::m_lock);
        vorti::applets::obs_plugin::m_shutting_down = true;
    }
    
    // Stop continuous mDNS discovery
    vorti::applets::obs_plugin::stop_continuous_discovery();
    
    // Notify all waiting threads to wake up and check shutdown flag
    vorti::applets::obs_plugin::m_compressor_ready_cv.notify_all();
    vorti::applets::obs_plugin::m_initialization_cv.notify_all();

    vorti::applets::obs_plugin::uninitialize_actions();

    obs_frontend_remove_event_callback(handle_obs_frontend_event, nullptr);
    obs_frontend_remove_save_callback(handle_obs_frontend_save, nullptr);

    // Cleanup banner functionality FIRST (before disconnecting)
    if constexpr (BANNER_MANAGER_ENABLED) {
        blog(LOG_INFO, "[OBS Plugin] Cleaning up banner manager...");
        // Call explicit cleanup method to stop all threads
        m_banner_manager.shutdown();
        
        // Unregister custom VortiDeck Banner source
        vorti::applets::banner_manager::unregister_vortideck_banner_source();
        blog(LOG_INFO, "[OBS Plugin] Banner manager cleanup complete");
    }

    vorti::applets::obs_plugin::disconnect();
    
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

    // Wait for connection with timeout
    {
        std::unique_lock<std::mutex> lk(m_compressor_ready_mutex);
        if (m_compressor_ready_cv.wait_until(lk, std::chrono::system_clock::now() + std::chrono::seconds(3)) == std::cv_status::timeout) {
            log_to_obs("Connection timeout - will retry later");
            disconnect();  // Clean up current attempt, but don't set m_shutting_down
            return false;  // This will trigger reconnection logic in _run_forever
        }
    }

    // Start initialization sequence
    log_to_obs("Connection established, starting initialization sequence...");
    
    // Reset failure counter on successful connection
    m_connection_failure_count = 0;

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
        if (m_initialization_cv.wait_until(lk, std::chrono::system_clock::now() + std::chrono::seconds(5)) == std::cv_status::timeout) {
            log_to_obs("Initialization timeout");
            disconnect();
            return false;
        }
        
        // Check if initialization was actually successful
        if (m_integration_instance.empty() || m_integration_guid.empty()) {
            log_to_obs("Initialization failed - missing integration details");
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
}


void vorti::applets::obs_plugin::websocket_message_handler(const websocketpp::connection_hdl &connection_handle,
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

void vorti::applets::obs_plugin::action_banner_set_data(const action_invoke_parameters &parameters)
{
    if constexpr (BANNER_MANAGER_ENABLED) {
        auto content_data_it = parameters.find("content_data");
        auto content_type_it = parameters.find("content_type");
        
        if (content_data_it != parameters.end() && content_type_it != parameters.end()) {
            log_to_obs("ACTION_BANNER_SET_DATA: Setting banner content");
            
            // Extract optional CSS and size parameters
            std::string custom_css = "";
            auto css_it = parameters.find("css");
            if (css_it != parameters.end()) {
                custom_css = css_it->second;
            }
            
            int custom_width = 0;
            int custom_height = 0;
            auto width_it = parameters.find("width");
            auto height_it = parameters.find("height");
            
            if (width_it != parameters.end()) {
                try {
                    custom_width = std::stoi(width_it->second);
                } catch (...) {
                    custom_width = 0;
                }
            }
            
            if (height_it != parameters.end()) {
                try {
                    custom_height = std::stoi(height_it->second);
                } catch (...) {
                    custom_height = 0;
                }
            }
            
            // Set banner content with parameters
            if (custom_css.empty() && custom_width == 0 && custom_height == 0) {
                m_banner_manager.set_banner_content(content_data_it->second, content_type_it->second);
            } else {
                m_banner_manager.set_banner_content_with_custom_params(
                    content_data_it->second, content_type_it->second, custom_css, 
                    custom_width, custom_height, false);
            }
            
            // Show banner
            m_banner_manager.show_banner();
            
            log_to_obs("ACTION_BANNER_SET_DATA: Banner content set successfully");
        } else {
            log_to_obs("ACTION_BANNER_SET_DATA: ERROR - Missing required parameters (content_data and content_type)");
        }
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
    
    if (m_continuous_discovery_thread.joinable()) {
        m_continuous_discovery_thread.join();
    }
    
    if (m_mdns_discovery) {
        m_mdns_discovery->stop_discovery();
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
                        
                        // TEMPORARY: Add hardcoded test service for selection window testing
                        ServiceInfo test_service;
                        test_service.name = "vortideck_test._vortideck._tcp.local.";
                        test_service.ip_address = "192.168.178.253";  // Use same IP as real service
                        test_service.port = 9002;  // Different port to distinguish from real service
                        test_service.websocket_url = "ws://192.168.178.253:9002/";
                        on_service_discovered(test_service);
                        log_to_obs("TESTING: Added hardcoded VortiDeck test service for selection testing");
                        
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
        should_show_dialog = (service_count >= 1 && !m_show_selection_dialog.exchange(true));
    }
    
    // Show dialog outside of mutex lock to avoid deadlock
    if (should_show_dialog) {
        log_to_obs(std::format("Multiple VortiDeck services discovered ({})", service_count));
        log_to_obs("DEBUG: About to call show_service_selection_dialog() directly");
        
        // Call the dialog without holding the mutex
        show_service_selection_dialog();
        
        log_to_obs("DEBUG: show_service_selection_dialog() call completed");
    } else if (service_count > 1) {
        log_to_obs("DEBUG: Multiple services but selection dialog already shown");
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
            return service.websocket_url;
        }
        
        // Use cached discovery URL if available
        if (!m_discovered_websocket_url.empty()) {
            log_to_obs(std::format("Using cached service: {}", m_discovered_websocket_url));
            return m_discovered_websocket_url;
        }
        
        // Use last known service if available
        if (!m_last_known_service_url.empty()) {
            log_to_obs(std::format("Using last known service: {}", m_last_known_service_url));
            return m_last_known_service_url;
        }
        
        // No service found - this will cause connection to fail, triggering retry
        log_to_obs("No VortiDeck service found - waiting for discovery");
        return "";
    } // Close mutex scope
}

void vorti::applets::obs_plugin::show_service_selection_dialog()
{
    log_to_obs("DEBUG: show_service_selection_dialog() STARTED");
    
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
        
        if (on_main_thread) {
            // We're already on the main thread, call directly
            log_to_obs("DEBUG: Creating dialog directly (already on main thread)");
            ServiceSelectionDialog dialog(services_copy, static_cast<QWidget*>(main_window));
            log_to_obs("DEBUG: ServiceSelectionDialog created successfully");
            log_to_obs("DEBUG: About to call dialog.exec()");
            result = dialog.exec();
            log_to_obs(std::format("DEBUG: dialog.exec() returned: {}", result));
            
            if (result == QDialog::Accepted) {
                selectedUrl = dialog.getSelectedServiceUrl();
                selectedIndex = dialog.getSelectedServiceIndex();
                log_to_obs("DEBUG: Retrieved dialog results");
            }
            dialog_completed = true;
        } else {
            // We're on a background thread, use invokeMethod
            QMetaObject::invokeMethod(app, [&]() {
                log_to_obs("DEBUG: Creating dialog on main thread via invokeMethod");
                ServiceSelectionDialog dialog(services_copy, static_cast<QWidget*>(main_window));
                log_to_obs("DEBUG: ServiceSelectionDialog created successfully on main thread");
                log_to_obs("DEBUG: About to call dialog.exec() on main thread");
                result = dialog.exec();
                log_to_obs(std::format("DEBUG: dialog.exec() returned: {} on main thread", result));
                
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
    QAction* banner_action = vortideck_menu->addAction("Banner Settings");
    QObject::connect(banner_action, &QAction::triggered, []() {
        log_to_obs("VortiDeck Banner Settings clicked from top-level menu");
        // Call existing banner functionality - you'll need to implement this
        // For now, just log that it was clicked
    });
    
    // Add Connection Settings submenu item  
    QAction* connection_action = vortideck_menu->addAction("Connection Settings");
    QObject::connect(connection_action, &QAction::triggered, []() {
        log_to_obs("VortiDeck Connection Settings clicked from top-level menu");
        if (g_obs_plugin_instance) {
            g_obs_plugin_instance->show_connection_settings_dialog();
        }
    });
    
    log_to_obs("✅ VortiDeck top-level menu created with Banner and Connection Settings");
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
    show_service_selection_dialog();
}





