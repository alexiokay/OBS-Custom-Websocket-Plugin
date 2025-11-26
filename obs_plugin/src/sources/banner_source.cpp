#include "banner_source.h"
#include "vortideck_common.h"
#include "../obs_plugin/src/banner_manager.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <thread>
#include <chrono>
#include <atomic>

// External functions to access banner_manager and WebSocket URL
extern vorti::applets::banner_manager& get_global_banner_manager();
extern std::string get_global_websocket_url();

struct banner_source {
    obs_source_t* source;
    obs_source_t* browser_source;  // The actual browser source (active child)
    std::string banner_id;
    bool triggered_banner_manager;

    // Self-management state
    signal_handler_t* scene_signal_handler;  // For monitoring scene item changes
    std::atomic<bool> correcting_enforcement;  // Prevent infinite loops
    std::atomic<bool> shutting_down;  // Shutdown flag
};

static const char* banner_source_get_name(void* type_data)
{
    UNUSED_PARAMETER(type_data);
    return "VortiDeck Banner";
}

// Helper: Check if enforcement is needed (queries banner_manager policy)
static bool should_enforce_banner(banner_source* context)
{
    if (!context) return false;

    try {
        auto& banner_mgr = get_global_banner_manager();
        // Only enforce for free users during streaming
        return !banner_mgr.is_premium_user() && obs_frontend_streaming_active();
    } catch (...) {
        return false;
    }
}

// Helper: Find this banner's scene item in a scene
static obs_sceneitem_t* find_banner_item_in_scene(obs_scene_t* scene, obs_source_t* banner_source)
{
    if (!scene || !banner_source) return nullptr;

    obs_sceneitem_t* found_item = nullptr;
    std::pair<obs_source_t*, obs_sceneitem_t**> search_data = {banner_source, &found_item};

    obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t* item, void* param) {
        auto* data = static_cast<std::pair<obs_source_t*, obs_sceneitem_t**>*>(param);
        obs_source_t* item_source = obs_sceneitem_get_source(item);

        if (item_source == data->first) {
            *(data->second) = item;
            return false;  // Stop enumeration
        }
        return true;  // Continue
    }, &search_data);

    return found_item;
}

// Helper: Enforce banner position and visibility (called by signal handlers)
static void enforce_banner_rules(banner_source* context)
{
    if (!context || context->shutting_down.load()) return;
    if (context->correcting_enforcement.load()) return;  // Prevent infinite loops

    if (!should_enforce_banner(context)) return;

    context->correcting_enforcement.store(true);

    // Get current scene
    obs_source_t* current_scene_source = obs_frontend_get_current_scene();
    if (!current_scene_source) {
        context->correcting_enforcement.store(false);
        return;
    }

    obs_scene_t* scene = obs_scene_from_source(current_scene_source);
    if (!scene) {
        obs_source_release(current_scene_source);
        context->correcting_enforcement.store(false);
        return;
    }

    // Find this banner in the scene
    obs_sceneitem_t* item = find_banner_item_in_scene(scene, context->source);
    if (item) {
        // Enforce visibility
        if (!obs_sceneitem_visible(item)) {
            obs_sceneitem_set_visible(item, true);
            blog(LOG_INFO, "[VortiDeck Banner] FREE USER: Enforced visibility");
        }

        // Enforce position (top of screen)
        vec2 pos;
        obs_sceneitem_get_pos(item, &pos);
        if (pos.x != 0.0f || pos.y != 0.0f) {
            pos.x = 0.0f;
            pos.y = 0.0f;
            obs_sceneitem_set_pos(item, &pos);
            blog(LOG_INFO, "[VortiDeck Banner] FREE USER: Enforced position (0,0)");
        }

        // Enforce locked state
        if (!obs_sceneitem_locked(item)) {
            obs_sceneitem_set_locked(item, true);
            blog(LOG_INFO, "[VortiDeck Banner] FREE USER: Enforced locked state");
        }

        // Enforce top z-order
        obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
    }

    obs_source_release(current_scene_source);
    context->correcting_enforcement.store(false);
}

// Signal handler: Item visibility changed
static void on_banner_item_visible(void* data, calldata_t* calldata)
{
    if (!data || !calldata) return;

    banner_source* context = static_cast<banner_source*>(data);
    if (context->shutting_down.load()) return;

    bool visible = calldata_bool(calldata, "visible");

    // If banner was hidden and we should enforce, restore it
    if (!visible && should_enforce_banner(context)) {
        blog(LOG_INFO, "[VortiDeck Banner] FREE USER: Banner hidden - enforcing visibility");
        enforce_banner_rules(context);
    }
}

// Signal handler: Item transform/position changed
static void on_banner_item_transform(void* data, calldata_t* calldata)
{
    if (!data || !calldata) return;

    banner_source* context = static_cast<banner_source*>(data);
    if (context->shutting_down.load()) return;
    if (context->correcting_enforcement.load()) return;  // Don't create infinite loop

    // If we should enforce, correct the position
    if (should_enforce_banner(context)) {
        blog(LOG_INFO, "[VortiDeck Banner] FREE USER: Banner moved - enforcing position");
        enforce_banner_rules(context);
    }
}

// Signal handler: Item locked state changed
static void on_banner_item_locked(void* data, calldata_t* calldata)
{
    if (!data || !calldata) return;

    banner_source* context = static_cast<banner_source*>(data);
    if (context->shutting_down.load()) return;

    bool locked = calldata_bool(calldata, "locked");

    // If banner was unlocked and we should enforce, lock it again
    if (!locked && should_enforce_banner(context)) {
        blog(LOG_INFO, "[VortiDeck Banner] FREE USER: Banner unlocked - enforcing locked state");
        enforce_banner_rules(context);
    }
}

static void* banner_source_create(obs_data_t* settings, obs_source_t* source)
{
    struct banner_source* context = new banner_source();
    context->source = source;
    context->browser_source = nullptr;
    context->banner_id = "menu_banner";
    context->triggered_banner_manager = false;
    context->scene_signal_handler = nullptr;
    context->correcting_enforcement.store(false);
    context->shutting_down.store(false);

    // Set source type metadata
    vortideck::set_source_type(source, vortideck::SourceType::ADS);

    blog(LOG_INFO, "[VortiDeck Banner] Creating self-managing banner source");

    // Get URL from settings or build default from WebSocket connection
    const char* url = obs_data_get_string(settings, "url");
    std::string final_url;
    if (!url || strlen(url) == 0) {
        // Build default URL from connected WebSocket server
        std::string websocket_url = get_global_websocket_url();
        if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
            // Convert WebSocket URL to HTTP URL and add /banners
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
                final_url = base_url + "banners";
            } else {
                final_url = base_url + "/banners";
            }
        } else {
            // Fallback if not a proper WebSocket URL
            final_url = websocket_url + "/banners";
        }
    } else {
        final_url = url;
    }

    // Create browser settings matching banner_manager's configuration
    obs_data_t* browser_settings = obs_data_create();
    obs_data_set_string(browser_settings, "url", final_url.c_str());
    obs_data_set_bool(browser_settings, "is_local_file", false);
    obs_data_set_bool(browser_settings, "restart_when_active", true);
    obs_data_set_int(browser_settings, "width", 1920);
    obs_data_set_int(browser_settings, "height", 100);

    // CRITICAL FIX: Control audio via OBS to prevent independent audio threads
    // Without reroute_audio=true, browser sources create independent audio threads that
    // don't respect OBS shutdown sequence, causing WASAPI crashes in copy_audio_data
    // when these threads try to access audio resources during teardown
    obs_data_set_bool(browser_settings, "reroute_audio", true);

    // Set shutdown=false - this parameter controls "Shutdown when not visible" behavior,
    // NOT CEF shutdown. CEF shutdowns automatically when the browser plugin unloads.
    obs_data_set_bool(browser_settings, "shutdown", false);

    // Create the browser source as a PRIVATE active child (like overlays do)
    context->browser_source = obs_source_create_private("browser_source",
        obs_source_get_name(source), browser_settings);

    if (context->browser_source) {
        // CRITICAL: Register as active child for proper CEF lifecycle management
        obs_source_add_active_child(source, context->browser_source);

        blog(LOG_INFO, "[VortiDeck Banner Menu] Created browser source as active child with URL: %s", final_url.c_str());

        // Notify banner_manager about the creation (for tracking purposes only)
        try {
            auto& banner_mgr = get_global_banner_manager();
            banner_mgr.set_banner_url(final_url);
            context->triggered_banner_manager = true;
        } catch (...) {
            blog(LOG_WARNING, "[VortiDeck Banner Menu] Failed to notify banner_manager");
        }
    } else {
        blog(LOG_ERROR, "[VortiDeck Banner Menu] Failed to create browser source");
    }

    obs_data_release(browser_settings);

    // Connect to scene signals for self-management (AFTER browser source is created)
    // This allows the banner to enforce its own visibility/position for free users
    obs_source_t* current_scene_source = obs_frontend_get_current_scene();
    if (current_scene_source) {
        obs_scene_t* scene = obs_scene_from_source(current_scene_source);
        if (scene) {
            // Connect to scene item signals so we can monitor THIS banner's changes
            context->scene_signal_handler = obs_source_get_signal_handler(current_scene_source);
            if (context->scene_signal_handler) {
                signal_handler_connect(context->scene_signal_handler, "item_visible", on_banner_item_visible, context);
                signal_handler_connect(context->scene_signal_handler, "item_transform", on_banner_item_transform, context);
                signal_handler_connect(context->scene_signal_handler, "item_locked", on_banner_item_locked, context);
                blog(LOG_INFO, "[VortiDeck Banner] Connected to scene signals for self-management");
            }
        }
        obs_source_release(current_scene_source);
    }

    return context;
}

static void banner_source_destroy(void* data)
{
    struct banner_source* context = (struct banner_source*)data;

    blog(LOG_INFO, "[VortiDeck Banner] Banner source destroyed - starting cleanup");

    // Set shutdown flag to stop all signal handlers immediately
    context->shutting_down.store(true);

    // Disconnect scene signals FIRST
    if (context->scene_signal_handler) {
        signal_handler_disconnect(context->scene_signal_handler, "item_visible", on_banner_item_visible, context);
        signal_handler_disconnect(context->scene_signal_handler, "item_transform", on_banner_item_transform, context);
        signal_handler_disconnect(context->scene_signal_handler, "item_locked", on_banner_item_locked, context);
        context->scene_signal_handler = nullptr;
        blog(LOG_INFO, "[VortiDeck Banner] Disconnected from scene signals");
    }

    // CRITICAL: Properly cleanup active child before destruction (like overlays do)
    if (context->browser_source) {
        obs_source_remove_active_child(context->source, context->browser_source);
        obs_source_release(context->browser_source);
        blog(LOG_INFO, "[VortiDeck Banner] Removed and released browser source active child");
    }

    blog(LOG_INFO, "[VortiDeck Banner] Banner source cleanup complete");
    delete context;
}

static void banner_source_update(void* data, obs_data_t* settings)
{
    struct banner_source* context = (struct banner_source*)data;

    // Update the browser source URL if it changed
    const char* new_url = obs_data_get_string(settings, "url");
    if (new_url && context->browser_source) {
        obs_data_t* browser_settings = obs_source_get_settings(context->browser_source);
        obs_data_set_string(browser_settings, "url", new_url);
        obs_source_update(context->browser_source, browser_settings);
        obs_data_release(browser_settings);

        blog(LOG_INFO, "[VortiDeck Banner Menu] Updated browser source URL: %s", new_url);

        // Notify banner_manager of URL change (for tracking)
        try {
            auto& banner_mgr = get_global_banner_manager();
            banner_mgr.set_banner_url(new_url);
        } catch (...) {
            blog(LOG_WARNING, "[VortiDeck Banner Menu] Failed to notify banner_manager of URL change");
        }
    }
}

static void banner_source_defaults(obs_data_t* settings)
{
    // Build default URL from connected WebSocket server
    std::string websocket_url = get_global_websocket_url();
    std::string default_url;
    
    if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
        // Convert WebSocket URL to HTTP URL and add /banners
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
            default_url = base_url + "banners";
        } else {
            default_url = base_url + "/banners";
        }
    } else {
        // Fallback if not a proper WebSocket URL
        default_url = websocket_url + "/banners";
    }
    
    obs_data_set_default_string(settings, "url", default_url.c_str());
}

static obs_properties_t* banner_source_properties(void* data)
{
    UNUSED_PARAMETER(data);
    
    obs_properties_t* props = obs_properties_create();
    
    // URL property
    obs_properties_add_text(props, "url", "Banner URL", OBS_TEXT_DEFAULT);
    
    // Add info text
    obs_property_t* info = obs_properties_add_text(props, "info", "Info", OBS_TEXT_INFO);
    obs_property_set_long_description(info, 
        "This triggers VortiDeck Banner Manager to show banners across all scenes.\n"
        "The banner will be automatically positioned, locked, and managed according to your account type.\n"
        "Premium users have more control over banner positioning and visibility.");
    
    return props;
}

// Render the browser source through the wrapper
static uint32_t banner_source_get_width(void* data)
{
    struct banner_source* context = (struct banner_source*)data;
    if (context->browser_source) {
        return obs_source_get_width(context->browser_source);
    }
    return 1920;  // Default width
}

static uint32_t banner_source_get_height(void* data)
{
    struct banner_source* context = (struct banner_source*)data;
    if (context->browser_source) {
        return obs_source_get_height(context->browser_source);
    }
    return 100;  // Default height
}

static void banner_source_render(void* data, gs_effect_t* effect)
{
    struct banner_source* context = (struct banner_source*)data;
    if (context->browser_source) {
        obs_source_video_render(context->browser_source);
    }
}

void register_banner_source()
{
    struct obs_source_info banner_info = {};
    banner_info.id = "vortideck_banner_menu";
    banner_info.type = OBS_SOURCE_TYPE_INPUT;
    banner_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    banner_info.get_name = banner_source_get_name;
    banner_info.create = banner_source_create;
    banner_info.destroy = banner_source_destroy;
    banner_info.update = banner_source_update;
    banner_info.get_defaults = banner_source_defaults;
    banner_info.get_properties = banner_source_properties;
    banner_info.get_width = banner_source_get_width;
    banner_info.get_height = banner_source_get_height;
    banner_info.video_render = banner_source_render;
    
    obs_register_source(&banner_info);
    blog(LOG_INFO, "VortiDeck Banner: Registered menu banner source that integrates with banner_manager");
}