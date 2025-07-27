#include "banner_source.h"
#include "vortideck_common.h"
#include "../obs_plugin/src/banner_manager.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <thread>
#include <chrono>

// External functions to access banner_manager and WebSocket URL
extern vorti::applets::banner_manager& get_global_banner_manager();
extern std::string get_global_websocket_url();

struct banner_source {
    obs_source_t* source;
    std::string banner_id;
    bool triggered_banner_manager;
};

static const char* banner_source_get_name(void* type_data)
{
    UNUSED_PARAMETER(type_data);
    return "VortiDeck Banner";
}

static void* banner_source_create(obs_data_t* settings, obs_source_t* source)
{
    struct banner_source* context = new banner_source();
    context->source = source;
    context->banner_id = "menu_banner";
    context->triggered_banner_manager = false;
    
    // Set source type metadata
    vortideck::set_source_type(source, vortideck::SourceType::ADS);
    
    // Get URL from settings or build default from WebSocket connection
    const char* url = obs_data_get_string(settings, "url");
    std::string final_url;
    if (!url || strlen(url) == 0) {
        // Build default URL from connected WebSocket server
        std::string websocket_url = get_global_websocket_url();
        if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
            // Convert WebSocket URL to HTTP URL and add /banners.html
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
                final_url = base_url + "banners.html";
            } else {
                final_url = base_url + "/banners.html";
            }
        } else {
            // Fallback if not a proper WebSocket URL
            final_url = websocket_url + "/banners.html";
        }
        url = final_url.c_str();
    }
    
    // Trigger banner_manager through global access function
    try {
        auto& banner_mgr = get_global_banner_manager();
        banner_mgr.set_banner_content(url, "url");
        banner_mgr.show_banner();
        context->triggered_banner_manager = true;
        
        blog(LOG_INFO, "[VortiDeck Banner Menu] Triggered banner_manager with URL: %s", url);
        
        // IMPORTANT: Remove this wrapper source from scenes after triggering banner_manager
        // The banner_manager will create its own private source
        std::thread([source]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Wait for banner_manager to finish
            
            // Find and remove this wrapper source from all scenes
            obs_frontend_source_list scenes = {};
            obs_frontend_get_scenes(&scenes);
            
            for (size_t i = 0; i < scenes.sources.num; i++) {
                obs_source_t* scene_source = scenes.sources.array[i];
                if (!scene_source) continue;
                
                obs_scene_t* scene = obs_scene_from_source(scene_source);
                if (!scene) continue;
                
                obs_sceneitem_t* item = obs_scene_find_source(scene, obs_source_get_name(source));
                if (item) {
                    obs_sceneitem_remove(item);
                    blog(LOG_INFO, "[VortiDeck Banner Menu] Removed wrapper source from scene");
                }
            }
            
            obs_frontend_source_list_free(&scenes);
        }).detach();
        
    } catch (...) {
        blog(LOG_WARNING, "[VortiDeck Banner Menu] Failed to access banner_manager");
    }
    
    return context;
}

static void banner_source_destroy(void* data)
{
    struct banner_source* context = (struct banner_source*)data;
    
    blog(LOG_INFO, "[VortiDeck Banner Menu] Banner menu source destroyed");
    // Note: We don't hide the banner on destroy since it should persist
    
    delete context;
}

static void banner_source_update(void* data, obs_data_t* settings)
{
    struct banner_source* context = (struct banner_source*)data;
    
    // Update banner content through banner_manager
    const char* new_url = obs_data_get_string(settings, "url");
    if (new_url) {
        try {
            auto& banner_mgr = get_global_banner_manager();
            banner_mgr.set_banner_content(new_url, "url");
            blog(LOG_INFO, "[VortiDeck Banner Menu] Updated banner URL: %s", new_url);
        } catch (...) {
            blog(LOG_WARNING, "[VortiDeck Banner Menu] Failed to update banner via banner_manager");
        }
    }
}

static void banner_source_defaults(obs_data_t* settings)
{
    // Build default URL from connected WebSocket server
    std::string websocket_url = get_global_websocket_url();
    std::string default_url;
    
    if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
        // Convert WebSocket URL to HTTP URL and add /banners.html
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
            default_url = base_url + "banners.html";
        } else {
            default_url = base_url + "/banners.html";
        }
    } else {
        // Fallback if not a proper WebSocket URL
        default_url = websocket_url + "/banners.html";
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

// These functions return 0 since the actual rendering is done by banner_manager
static uint32_t banner_source_get_width(void* data)
{
    UNUSED_PARAMETER(data);
    return 0; // No direct rendering - banner_manager handles this
}

static uint32_t banner_source_get_height(void* data)
{
    UNUSED_PARAMETER(data);
    return 0; // No direct rendering - banner_manager handles this
}

static void banner_source_render(void* data, gs_effect_t* effect)
{
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(effect);
    // No direct rendering - banner_manager handles the actual banner display
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