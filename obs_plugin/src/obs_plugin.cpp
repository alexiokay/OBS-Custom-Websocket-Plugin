#include "obs_plugin.hpp"

#include <UI/obs-frontend-api/obs-frontend-api.h>
#include <libobs/obs-module.h>
#include <libobs/util/platform.h>

using namespace vorti::applets::obs_plugin;

/*
Helpful resources: https://obsproject.com/docs/reference-modules.html
*/

/* Defines common functions (required) */
OBS_DECLARE_MODULE()


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
    
    // Start connection in a separate thread to avoid blocking OBS startup
    std::thread([]{
        // Add a small delay to let OBS finish initializing
        std::this_thread::sleep_for(std::chrono::seconds(1));
        connect();
    }).detach();

    return true;
}


static void handle_obs_frontend_save(obs_data_t *save_data, bool saving, void *)
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
    register_regular_actions();

    obs_frontend_add_event_callback(handle_obs_frontend_event, nullptr);
    obs_frontend_add_save_callback(handle_obs_frontend_save, nullptr);

    register_actions_broadcast();

    start_loop();
}


void obs_module_unload()
{
    // Called when the module is unloaded.
    uninitialize_actions();

    obs_frontend_remove_event_callback(handle_obs_frontend_event, nullptr);
    obs_frontend_remove_save_callback(handle_obs_frontend_save, nullptr);

    disconnect();
}
void log_to_obs(const std::string &message) {
    blog(LOG_INFO, message.c_str());  // Log to OBS
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
            m_websocket_thread.reset(new std::thread(&_run_forever));
        }
    }

    // Wait for connection with timeout
    {
        std::unique_lock<std::mutex> lk(m_compressor_ready_mutex);
        if (m_compressor_ready_cv.wait_until(lk, std::chrono::system_clock::now() + std::chrono::seconds(3)) == std::cv_status::timeout) {
            log_to_obs("Connection timeout - cleaning up");
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
        if (m_initialization_cv.wait_until(lk, std::chrono::system_clock::now() + std::chrono::seconds(5)) == std::cv_status::timeout) {
            log_to_obs("Initialization timeout");
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
            log_to_obs("Reconnection attempt " + std::to_string(attempt + 1));
            
            // Attempt to establish new connection with full initialization
            if (connect()) {
                log_to_obs("Reconnection successful");
                return;
            }
            
            attempt++;
            std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));

        } catch (const std::exception& e) {
            log_to_obs("Reconnection attempt failed: " + std::string(e.what()));
            attempt++;
            std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
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

    // Notify any waiting threads
    m_compressor_ready_cv.notify_all();
    m_initialization_cv.notify_all();

    // Release all locks before attempting reconnection
    if (should_reconnect && !m_shutting_down) {
        // Create a new thread without capturing this
        std::thread([]() {
            try {
                // Call the static reconnect function
                vorti::applets::obs_plugin::reconnect();
            } catch (const std::exception& e) {
                log_to_obs("Reconnection failed: " + std::string(e.what()));
            }
        }).detach();
    }
}


void vorti::applets::obs_plugin::disconnect()
{
    // Set connection state first
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
        m_integration_guid.clear();
        m_integration_instance.clear();
        m_current_message_id = 1;
    }

    // Stop the status update loop first
    stop_loop();

    // Then stop the websocket and thread
    if (m_websocket_thread)
    {
        try {
            m_websocket.stop();
            if (m_websocket_thread->joinable())
            {
                m_websocket_thread->join();
            }
        } catch (const std::exception& e) {
            log_to_obs("Error during WebSocket shutdown: " + std::string(e.what()));
        }
        m_websocket_thread.reset();
    }

    // Notify any waiting threads
    m_compressor_ready_cv.notify_all();
    m_initialization_cv.notify_all();
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
    }

    while (!m_shutting_down)
    {
        try
        {
            websocketpp::lib::error_code ec;
            ws_client::connection_ptr connection = _create_connection();
            
            if (!connection) {
                log_to_obs("Failed to create connection");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // Connect and run
            m_websocket.connect(connection);
            
            try {
                m_websocket.run();
            } catch (const std::exception& e) {
                log_to_obs("Exception in WebSocket run: " + std::string(e.what()));
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
            
            // Small delay before next attempt
            if (!m_shutting_down) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
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
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}


ws_client::connection_ptr vorti::applets::obs_plugin::_create_connection()
{
    websocketpp::lib::error_code error_code;
    auto connection = m_websocket.get_connection("ws://192.168.178.129:" + std::to_string(m_current_port), error_code);

    assert(connection || 0 != error_code.value());
    if (!connection || 0 != error_code.value())
    {
        return connection;
    }

    connection->set_open_handler([](const websocketpp::connection_hdl &connection_handle) {
        websocket_open_handler(connection_handle);
    });
    connection->set_message_handler(
        [](const websocketpp::connection_hdl &connection_handle, const ws_client::message_ptr &response) {
            websocket_message_handler(connection_handle, response);
        });
    connection->set_close_handler([](const websocketpp::connection_hdl &connection_handle) {
        websocket_close_handler(connection_handle);
    });
    connection->set_fail_handler([](const websocketpp::connection_hdl &connection_handle) {
        websocket_fail_handler(connection_handle);
    });

    if (!m_subprotocol.empty())
    {
        connection->add_subprotocol(m_subprotocol);
    }

    m_connection_handle = connection->get_handle();

    return connection;
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

        if (!message["result"].is_null() && (message["result"]["code"].get<std::string>() != "SUCCESS"))
        {
            log_to_obs("Error received in result: " + message["result"]["code"].get<std::string>());
            return;
        }

        // Handle registration response first
        if ((message["verb"].get<std::string>() == "SET")
            && (message["path"].get<std::string>() == "/api/v1/integration/register"))
        {
            log_to_obs("Registration response received, initializing actions...");
            if (!initialize_actions()) {
                log_to_obs("Failed to initialize actions after registration");
                return;
            }
            return;
        }

        // Handle activation response
        if ((message["verb"].get<std::string>() == "SET")
            && (message["path"].get<std::string>() == "/api/v1/integration/activate"))
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

        // Rest of the message handling...

        if ((message["verb"].get<std::string>() == "BROADCAST")
            && (message["path"].get<std::string>() == "/api/v1/integration/sdk/action/invoke"))
        {
            // log_to_obs("Received BROADCAST /api/v1/integration/sdk/action/invoke");

            auto invoked_action = message["payload"];

            if (!invoked_action.is_null())
            {
                // log_to_obs("Invoked action: " + invoked_action.dump());
                // log_to_obs("Integration GUID: " + m_integration_guid);

                if (invoked_action["integrationGuid"] == m_integration_guid)
                {
                    // log_to_obs("Integration GUID matches: " + m_integration_guid);

                    action_invoke_parameters parameters = invoked_action["parameters"];
                    std::string action_id = invoked_action["actionId"];
                    // log_to_obs("Action ID: " + action_id);


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
                }
            }
        }
    }
    catch (std::exception e)
    {
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

    // Await initialization details
    std::unique_lock<std::mutex> lk(m_initialization_mutex);
    m_initialization_cv.wait_until(lk, std::chrono::system_clock::now() + std::chrono::seconds(1));

    return !m_integration_instance.empty() && !m_integration_guid.empty();
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
        m_loop_thread = std::thread(&loop_function);
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

    // Stop the thread
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
    }

    // Cleanup
    if (cpu_usage != nullptr)
    {
        os_cpu_usage_info_destroy(cpu_usage);
    }
}
