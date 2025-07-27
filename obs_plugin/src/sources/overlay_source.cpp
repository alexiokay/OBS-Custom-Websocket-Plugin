#include "overlay_source.h"
#include "vortideck_common.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>

// External function to get WebSocket URL
extern std::string get_global_websocket_url();
#include <string>
#include <thread>
#include <chrono>
#include <QJsonObject>
#include <QJsonDocument>

struct overlay_source {
    obs_source_t* source;
    obs_source_t* browser_source;
    std::string overlay_id;
    std::string url;
    bool auto_resize_enabled;
    signal_handler_t* video_signal_handler;
};

// Lock and configure scene item for main overlay
static void lock_overlay_item(obs_sceneitem_t* item, const char* overlay_id, uint32_t width, uint32_t height)
{
    if (!item) return;
    
    bool is_main = overlay_id && strcmp(overlay_id, "main_overlay") == 0;
    
    if (is_main) {
        // Force position to 0,0
        struct vec2 pos = {0.0f, 0.0f};
        obs_sceneitem_set_pos(item, &pos);
        
        // Set bounds to canvas size for stretching
        struct vec2 bounds;
        bounds.x = (float)width;
        bounds.y = (float)height;
        
        obs_sceneitem_set_bounds(item, &bounds);
        obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
        obs_sceneitem_set_bounds_alignment(item, 0); // Top-left alignment
        
        // Lock the item to prevent manipulation
        obs_sceneitem_set_locked(item, true);
        
        // Set overlay above regular sources but below banners (ADS)
        // First move to top, then banners will be moved above this
        obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
        
        blog(LOG_INFO, "[VortiDeck Overlay] Locked main overlay at %dx%d", width, height);
    }
}

// Update scene item bounds for auto-resize
static void update_scene_item_bounds(obs_sceneitem_t* item, uint32_t width, uint32_t height)
{
    if (!item) return;
    
    // Set bounds to canvas size
    struct vec2 bounds;
    bounds.x = (float)width;
    bounds.y = (float)height;
    
    obs_sceneitem_set_bounds(item, &bounds);
    obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
    obs_sceneitem_set_bounds_alignment(item, 0); // Top-left alignment
    
    // Ensure position is 0,0
    struct vec2 pos = {0.0f, 0.0f};
    obs_sceneitem_set_pos(item, &pos);
    
    blog(LOG_INFO, "[VortiDeck Overlay] Updated scene item bounds to %dx%d", width, height);
}

// Callback for scene enumeration
static bool find_and_update_overlay_items(obs_scene_t* scene, obs_sceneitem_t* item, void* data)
{
    struct overlay_source* context = (struct overlay_source*)data;
    obs_source_t* item_source = obs_sceneitem_get_source(item);
    
    if (item_source == context->source) {
        obs_video_info ovi;
        if (obs_get_video_info(&ovi)) {
            // For main overlay, enforce locking
            if (context->overlay_id == "main_overlay") {
                lock_overlay_item(item, context->overlay_id.c_str(), ovi.base_width, ovi.base_height);
            } else {
                update_scene_item_bounds(item, ovi.base_width, ovi.base_height);
            }
        }
    }
    
    return true; // Continue enumeration
}

// Canvas resize signal handler
static void handle_canvas_resize(void* data, calldata_t* cd)
{
    UNUSED_PARAMETER(cd);
    struct overlay_source* context = (struct overlay_source*)data;
    
    if (!context || !context->auto_resize_enabled) {
        return;
    }
    
    // Get current canvas size
    obs_video_info ovi;
    if (!obs_get_video_info(&ovi)) {
        blog(LOG_WARNING, "[VortiDeck Overlay] Failed to get video info for resize");
        return;
    }
    
    blog(LOG_INFO, "[VortiDeck Overlay] Canvas resize detected: %dx%d", ovi.base_width, ovi.base_height);
    
    // Update browser source dimensions to match canvas
    if (context->browser_source) {
        obs_data_t* browser_settings = obs_source_get_settings(context->browser_source);
        
        // Check if dimensions actually changed
        int old_width = (int)obs_data_get_int(browser_settings, "width");
        int old_height = (int)obs_data_get_int(browser_settings, "height");
        
        if (old_width != ovi.base_width || old_height != ovi.base_height) {
            blog(LOG_INFO, "[VortiDeck Overlay] Resolution changed from %dx%d to %dx%d - recreating browser source", 
                 old_width, old_height, ovi.base_width, ovi.base_height);
            
            // Get current URL and settings before recreating
            const char* current_url = obs_data_get_string(browser_settings, "url");
            const char* current_css = obs_data_get_string(browser_settings, "css");
            std::string url_copy = current_url ? current_url : "";
            std::string css_copy = current_css ? current_css : "";
            
            // Remove old browser source
            obs_source_remove_active_child(context->source, context->browser_source);
            obs_source_release(context->browser_source);
            
            // Create new browser source with correct dimensions
            obs_data_t* new_settings = obs_data_create();
            obs_data_set_string(new_settings, "url", url_copy.c_str());
            obs_data_set_int(new_settings, "width", ovi.base_width);
            obs_data_set_int(new_settings, "height", ovi.base_height);
            obs_data_set_bool(new_settings, "reroute_audio", true);
            obs_data_set_bool(new_settings, "shutdown", false);
            obs_data_set_int(new_settings, "fps", 30);
            if (!css_copy.empty()) {
                obs_data_set_string(new_settings, "css", css_copy.c_str());
            }
            
            context->browser_source = obs_source_create_private("browser_source", 
                obs_source_get_name(context->source), new_settings);
            
            if (context->browser_source) {
                obs_source_add_active_child(context->source, context->browser_source);
                blog(LOG_INFO, "[VortiDeck Overlay] Successfully recreated browser source with %dx%d", 
                     ovi.base_width, ovi.base_height);
            } else {
                blog(LOG_ERROR, "[VortiDeck Overlay] Failed to recreate browser source!");
            }
            
            obs_data_release(new_settings);
        } else {
            // No dimension change, just normal update
            obs_source_update(context->browser_source, browser_settings);
        }
        
        obs_data_release(browser_settings);
        
        blog(LOG_INFO, "[VortiDeck Overlay] Updated browser source to %dx%d", ovi.base_width, ovi.base_height);
    }
    
    // Update all scene items using this source
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* scene_source = scenes.sources.array[i];
        if (!scene_source) continue;
        
        obs_scene_t* scene = obs_scene_from_source(scene_source);
        if (!scene) continue;
        
        obs_scene_enum_items(scene, find_and_update_overlay_items, context);
    }
    
    obs_frontend_source_list_free(&scenes);
}


static const char* overlay_source_get_name(void* type_data)
{
    UNUSED_PARAMETER(type_data);
    return "VortiDeck Overlay";
}

static void* overlay_source_create(obs_data_t* settings, obs_source_t* source)
{
    struct overlay_source* context = new overlay_source();
    context->source = source;
    context->browser_source = nullptr;
    context->auto_resize_enabled = false;
    context->video_signal_handler = nullptr;
    
    // Set source type metadata
    vortideck::set_source_type(source, vortideck::SourceType::OVERLAY);
    
    // Get overlay configuration from settings
    const char* overlay_id = obs_data_get_string(settings, "overlay_id");
    const char* url = obs_data_get_string(settings, "url");
    int width = (int)obs_data_get_int(settings, "width");
    int height = (int)obs_data_get_int(settings, "height");
    
    blog(LOG_INFO, "[VortiDeck Overlay] STARTUP DEBUG: Initial values - width=%d, height=%d, overlay_id=%s, url=%s", 
         width, height, overlay_id ? overlay_id : "null", url ? url : "null");
    
    if (!overlay_id || strlen(overlay_id) == 0) {
        overlay_id = "default";
    }
    
    context->overlay_id = overlay_id;
    context->url = url ? url : "";
    
    // Get canvas size if auto-resize will be enabled
    bool will_auto_resize = obs_data_get_bool(settings, "auto_resize") || 
                           (context->overlay_id == "main_overlay");
    
    blog(LOG_INFO, "[VortiDeck Overlay] STARTUP DEBUG: will_auto_resize=%s, is_main=%s", 
         will_auto_resize ? "true" : "false", 
         (context->overlay_id == "main_overlay") ? "true" : "false");
    
    if (will_auto_resize) {
        obs_video_info ovi;
        if (obs_get_video_info(&ovi)) {
            width = ovi.base_width;
            height = ovi.base_height;
            blog(LOG_INFO, "[VortiDeck Overlay] Auto-resize enabled: Using current canvas size %dx%d", width, height);
        }
    }
    
    // Create browser source with appropriate settings
    obs_data_t* browser_settings = obs_data_create();
    obs_data_set_string(browser_settings, "url", url);
    obs_data_set_int(browser_settings, "width", width > 0 ? width : 1920);
    obs_data_set_int(browser_settings, "height", height > 0 ? height : 1080);
    obs_data_set_bool(browser_settings, "reroute_audio", true);
    obs_data_set_bool(browser_settings, "shutdown", false);
    obs_data_set_int(browser_settings, "fps", 30);
    
    // Copy any custom CSS
    const char* css = obs_data_get_string(settings, "css");
    if (css && strlen(css) > 0) {
        obs_data_set_string(browser_settings, "css", css);
    }
    
    // Create the browser source
    context->browser_source = obs_source_create_private("browser_source", 
        obs_source_get_name(source), browser_settings);
    
    if (context->browser_source) {
        // Add as child source for proper resource management
        obs_source_add_active_child(source, context->browser_source);
    }
    
    obs_data_release(browser_settings);
    
    // Store overlay ID in private settings
    obs_data_t* private_settings = obs_source_get_private_settings(source);
    obs_data_set_string(private_settings, vortideck::META_ID, overlay_id);
    obs_data_release(private_settings);
    
    // Check if auto-resize is enabled (force true for main overlay)
    bool auto_resize = obs_data_get_bool(settings, "auto_resize");
    
    // Force auto-resize for main overlay
    if (strcmp(context->overlay_id.c_str(), "main_overlay") == 0) {
        auto_resize = true;
        obs_data_set_bool(settings, "auto_resize", true);
    }
    
    context->auto_resize_enabled = auto_resize;
    
    // Connect to video reset signals if auto-resize enabled
    if (auto_resize) {
        signal_handler_t* obs_signals = obs_get_signal_handler();
        if (obs_signals) {
            context->video_signal_handler = obs_signals;
            signal_handler_connect(obs_signals, "video_reset", handle_canvas_resize, context);
            signal_handler_connect(obs_signals, "canvas_video_reset", handle_canvas_resize, context);
            
            blog(LOG_INFO, "[VortiDeck Overlay] Connected to video reset signals for auto-resize (overlay_id: %s)", context->overlay_id.c_str());
            
            // Immediately resize to current canvas
            handle_canvas_resize(context, nullptr);
        } else {
            blog(LOG_WARNING, "[VortiDeck Overlay] Failed to get signal handler for auto-resize");
        }
    }
    
    return context;
}

static void overlay_source_destroy(void* data)
{
    struct overlay_source* context = (struct overlay_source*)data;
    
    // Disconnect video signals if connected
    if (context->video_signal_handler && context->auto_resize_enabled) {
        signal_handler_disconnect(context->video_signal_handler, "video_reset", handle_canvas_resize, context);
        signal_handler_disconnect(context->video_signal_handler, "canvas_video_reset", handle_canvas_resize, context);
    }
    
    if (context->browser_source) {
        obs_source_remove_active_child(context->source, context->browser_source);
        obs_source_release(context->browser_source);
    }
    
    delete context;
}

static void overlay_source_update(void* data, obs_data_t* settings)
{
    struct overlay_source* context = (struct overlay_source*)data;
    
    // Check if forced browser recreation is requested (from VortiDeck resolution update)
    bool force_recreation = obs_data_get_bool(settings, "force_browser_recreation");
    if (force_recreation) {
        blog(LOG_INFO, "[VortiDeck Overlay] FORCE_RECREATION: VortiDeck resolution update detected");
        
        // Clear the flag to avoid repeated recreation
        obs_data_set_bool(settings, "force_browser_recreation", false);
        
        // Force browser source recreation with new dimensions
        if (context->browser_source) {
            int new_width = (int)obs_data_get_int(settings, "width");
            int new_height = (int)obs_data_get_int(settings, "height");
            const char* current_url = obs_data_get_string(settings, "url");
            const char* current_css = obs_data_get_string(settings, "css");
            
            blog(LOG_INFO, "[VortiDeck Overlay] FORCE_RECREATION: Recreating browser source for %dx%d", new_width, new_height);
            
            // Remove old browser source
            obs_source_remove_active_child(context->source, context->browser_source);
            obs_source_release(context->browser_source);
            
            // Create new browser source with correct dimensions
            obs_data_t* browser_settings = obs_data_create();
            obs_data_set_string(browser_settings, "url", current_url ? current_url : "");
            obs_data_set_int(browser_settings, "width", new_width);
            obs_data_set_int(browser_settings, "height", new_height);
            obs_data_set_bool(browser_settings, "reroute_audio", true);
            obs_data_set_bool(browser_settings, "shutdown", false);
            obs_data_set_int(browser_settings, "fps", 30);
            if (current_css && strlen(current_css) > 0) {
                obs_data_set_string(browser_settings, "css", current_css);
            }
            
            context->browser_source = obs_source_create_private("browser_source", 
                obs_source_get_name(context->source), browser_settings);
            
            if (context->browser_source) {
                obs_source_add_active_child(context->source, context->browser_source);
                blog(LOG_INFO, "[VortiDeck Overlay] FORCE_RECREATION: Successfully created new browser source");
            } else {
                blog(LOG_ERROR, "[VortiDeck Overlay] FORCE_RECREATION: Failed to create new browser source!");
            }
            
            obs_data_release(browser_settings);
        }
        
        // Update scene items as well
        handle_canvas_resize(context, nullptr);
        return;
    }
    
    // Update URL if changed
    const char* new_url = obs_data_get_string(settings, "url");
    if (new_url && context->url != new_url) {
        blog(LOG_INFO, "[VortiDeck Overlay] URL changed from '%s' to '%s'", 
             context->url.c_str(), new_url);
        context->url = new_url;
        
        if (context->browser_source) {
            obs_data_t* browser_settings = obs_data_create();
            obs_data_set_string(browser_settings, "url", new_url);
            obs_source_update(context->browser_source, browser_settings);
            obs_data_release(browser_settings);
            blog(LOG_INFO, "[VortiDeck Overlay] Updated browser source URL to: %s", new_url);
        } else {
            blog(LOG_WARNING, "[VortiDeck Overlay] No browser source to update!");
        }
    }
    
    // Check if this is the main overlay
    bool is_main_overlay = (context->overlay_id == "main_overlay");
    
    // Update auto-resize setting
    bool auto_resize = obs_data_get_bool(settings, "auto_resize");
    
    // Force auto-resize for main overlay
    if (is_main_overlay) {
        auto_resize = true;
        obs_data_set_bool(settings, "auto_resize", true);
    }
    
    bool was_auto_resize = context->auto_resize_enabled;
    context->auto_resize_enabled = auto_resize;
    
    // Handle signal connection changes
    if (auto_resize != was_auto_resize) {
        signal_handler_t* obs_signals = obs_get_signal_handler();
        if (obs_signals) {
            if (auto_resize) {
                // Connect signals
                context->video_signal_handler = obs_signals;
                signal_handler_connect(obs_signals, "video_reset", handle_canvas_resize, context);
                signal_handler_connect(obs_signals, "canvas_video_reset", handle_canvas_resize, context);
                // Immediately resize to canvas
                handle_canvas_resize(context, nullptr);
            } else {
                // Disconnect signals
                signal_handler_disconnect(obs_signals, "video_reset", handle_canvas_resize, context);
                signal_handler_disconnect(obs_signals, "canvas_video_reset", handle_canvas_resize, context);
                context->video_signal_handler = nullptr;
            }
        }
    }
    
    // Update dimensions
    if (auto_resize) {
        // If auto-resize enabled, immediately resize to canvas
        handle_canvas_resize(context, nullptr);
    } else {
        // Manual dimensions
        int width = (int)obs_data_get_int(settings, "width");
        int height = (int)obs_data_get_int(settings, "height");
        
        if (context->browser_source && (width > 0 || height > 0)) {
            obs_data_t* browser_settings = obs_source_get_settings(context->browser_source);
            if (width > 0) obs_data_set_int(browser_settings, "width", width);
            if (height > 0) obs_data_set_int(browser_settings, "height", height);
            obs_source_update(context->browser_source, browser_settings);
            obs_data_release(browser_settings);
        }
    }
}

static void overlay_source_defaults(obs_data_t* settings)
{
    // Get current canvas size for defaults
    obs_video_info ovi;
    uint32_t width = 1920;
    uint32_t height = 1080;
    if (obs_get_video_info(&ovi)) {
        width = ovi.base_width;
        height = ovi.base_height;
    }
    
    obs_data_set_default_int(settings, "width", width);
    obs_data_set_default_int(settings, "height", height);
    
    // Build default URL from connected WebSocket server + /overlay.html
    std::string websocket_url = get_global_websocket_url();
    std::string default_url;
    
    if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
        // Convert WebSocket URL to HTTP URL and add /overlay.html
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
            default_url = base_url + "overlay.html";
        } else {
            default_url = base_url + "/overlay.html";
        }
    } else {
        // Fallback if not a proper WebSocket URL
        default_url = websocket_url + "/overlay.html";
    }
    
    blog(LOG_INFO, "[VortiDeck Overlay] Using connected server URL: %s", default_url.c_str());
    
    obs_data_set_default_string(settings, "url", default_url.c_str());
    obs_data_set_default_string(settings, "overlay_id", "main_overlay");
    
    // Check if this is the main overlay
    const char* overlay_id = obs_data_get_string(settings, "overlay_id");
    bool is_main = overlay_id && strcmp(overlay_id, "main_overlay") == 0;
    obs_data_set_default_bool(settings, "auto_resize", is_main);
}

static bool auto_resize_modified(obs_properties_t* props, obs_property_t* property, obs_data_t* settings)
{
    UNUSED_PARAMETER(property);
    bool auto_resize = obs_data_get_bool(settings, "auto_resize");
    
    obs_property_t* width_prop = obs_properties_get(props, "width");
    obs_property_t* height_prop = obs_properties_get(props, "height");
    
    if (width_prop) obs_property_set_visible(width_prop, !auto_resize);
    if (height_prop) obs_property_set_visible(height_prop, !auto_resize);
    
    return true;
}

static obs_properties_t* overlay_source_properties(void* data)
{
    struct overlay_source* context = (struct overlay_source*)data;
    bool is_main_overlay = context && context->overlay_id == "main_overlay";
    
    obs_properties_t* props = obs_properties_create();
    
    // Overlay ID (read-only for main overlay)
    obs_property_t* id_prop = obs_properties_add_text(props, "overlay_id", "Overlay ID", OBS_TEXT_DEFAULT);
    if (is_main_overlay) {
        obs_property_set_enabled(id_prop, false);
    }
    
    obs_properties_add_text(props, "url", "URL", OBS_TEXT_DEFAULT);
    
    // Auto-resize (disabled and forced on for main overlay)
    obs_property_t* auto_resize_prop = obs_properties_add_bool(props, "auto_resize", "Auto-resize to Canvas");
    obs_property_set_modified_callback(auto_resize_prop, auto_resize_modified);
    if (is_main_overlay) {
        obs_property_set_enabled(auto_resize_prop, false);
    }
    
    obs_properties_add_int(props, "width", "Width", 1, 3840, 1);
    obs_properties_add_int(props, "height", "Height", 1, 2160, 1);
    obs_properties_add_text(props, "css", "Custom CSS", OBS_TEXT_MULTILINE);
    
    return props;
}

static uint32_t overlay_source_get_width(void* data)
{
    struct overlay_source* context = (struct overlay_source*)data;
    return context->browser_source ? obs_source_get_width(context->browser_source) : 0;
}

static uint32_t overlay_source_get_height(void* data)
{
    struct overlay_source* context = (struct overlay_source*)data;
    return context->browser_source ? obs_source_get_height(context->browser_source) : 0;
}

static void overlay_source_render(void* data, gs_effect_t* effect)
{
    struct overlay_source* context = (struct overlay_source*)data;
    
    if (context->browser_source) {
        obs_source_video_render(context->browser_source);
    }
}

static void overlay_source_enum_active_sources(void* data, obs_source_enum_proc_t enum_callback, void* param)
{
    struct overlay_source* context = (struct overlay_source*)data;
    
    if (context->browser_source) {
        enum_callback(context->source, context->browser_source, param);
    }
}

void register_overlay_source()
{
    struct obs_source_info overlay_info = {};
    overlay_info.id = vortideck::SOURCE_ID_OVERLAY;
    overlay_info.type = OBS_SOURCE_TYPE_INPUT;
    overlay_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
    overlay_info.get_name = overlay_source_get_name;
    overlay_info.create = overlay_source_create;
    overlay_info.destroy = overlay_source_destroy;
    overlay_info.update = overlay_source_update;
    overlay_info.get_defaults = overlay_source_defaults;
    overlay_info.get_properties = overlay_source_properties;
    overlay_info.get_width = overlay_source_get_width;
    overlay_info.get_height = overlay_source_get_height;
    overlay_info.video_render = overlay_source_render;
    overlay_info.enum_active_sources = overlay_source_enum_active_sources;
    
    obs_register_source(&overlay_info);
}