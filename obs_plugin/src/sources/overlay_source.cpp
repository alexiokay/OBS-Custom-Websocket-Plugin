#include "overlay_source.h"
#include "vortideck_common.h"
#include "../preview_feed.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>

// The overlay URL is a host-issued capability. Discovery URLs never contain it.
extern std::string get_global_overlay_capability_url();
#include <mutex>
#include <string>
#include <vector>

struct overlay_source {
    obs_source_t* source;
    // Written only on the UI thread (create, resize, destroy) but read by the
    // graphics thread while rendering, so replacements swap under this lock
    // and readers take their own reference.
    obs_source_t* browser_source;
    std::mutex browser_mutex;
    std::string overlay_id;
    std::string url;
    bool auto_resize_enabled;
};

// A reference to the current browser child, safe on any thread; release it.
static obs_source_t* acquire_browser(overlay_source* context)
{
    std::lock_guard<std::mutex> lock(context->browser_mutex);
    return context->browser_source ? obs_source_get_ref(context->browser_source) : nullptr;
}

// Replace the browser child (UI thread). The previous one is released after
// the swap, outside the lock, so rendering never sees a freed source.
static void replace_browser(overlay_source* context, obs_source_t* replacement)
{
    obs_source_t* previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(context->browser_mutex);
        previous = context->browser_source;
        context->browser_source = replacement;
    }
    if (replacement) obs_source_add_active_child(context->source, replacement);
    if (previous) {
        obs_source_remove_active_child(context->source, previous);
        obs_source_release(previous);
    }
}

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
        obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
        
        // Lock the item to prevent manipulation
        obs_sceneitem_set_locked(item, true);
        
        // Preserve the scene's chosen source order.
        
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
    obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
    
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
    
    if (obs_sceneitem_is_group(item)) {
        obs_scene_enum_items(obs_sceneitem_group_get_scene(item), find_and_update_overlay_items, data);
    }
    return true; // Continue enumeration
}

// Source updates and the companion observer share this one resize path.
static void handle_canvas_resize(overlay_source* context)
{
    
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
            
            obs_data_set_int(browser_settings, "width", ovi.base_width);
            obs_data_set_int(browser_settings, "height", ovi.base_height);
            obs_source_t* replacement = obs_source_create_private("browser_source",
                obs_source_get_name(context->source), browser_settings);
            if (!replacement) {
                obs_data_release(browser_settings);
                blog(LOG_ERROR, "[VortiDeck Overlay] Resize failed; keeping the previous browser source");
                return;
            }
            replace_browser(context, replacement);
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
    
    // Set source type metadata
    vortideck::set_source_type(source, vortideck::SourceType::OVERLAY);
    
    // Get overlay configuration from settings
    const char* overlay_id = obs_data_get_string(settings, "overlay_id");
    // Overlay tokens are scoped to the running VortiDeck host. A URL restored
    // from an OBS scene collection therefore cannot be trusted after either
    // process restarts. Prefer the currently connected host capability and
    // otherwise keep the private browser source inert until set_data arrives.
    std::string capability_url = get_global_overlay_capability_url();
    const char* url = capability_url.empty() ? "about:blank" : capability_url.c_str();
    obs_data_set_string(settings, "url", url);
    int width = (int)obs_data_get_int(settings, "width");
    int height = (int)obs_data_get_int(settings, "height");
    
    // The URL is a bearer capability and must never be written to OBS logs.
    if (capability_url.empty()) {
        blog(LOG_INFO, "[VortiDeck Overlay] Waiting for an authenticated overlay capability");
    } else {
        blog(LOG_INFO, "[VortiDeck Overlay] Using the current authenticated overlay capability");
    }
    blog(LOG_INFO, "[VortiDeck Overlay] Initializing source - width=%d, height=%d, overlay_id=%s",
         width, height, overlay_id ? overlay_id : "null");
    
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
    
    // One companion UI-thread observer owns resize detection. Initial source
    // settings already use the current canvas when auto-resize is enabled.
    return context;
}

static void overlay_source_destroy(void* data)
{
    struct overlay_source* context = (struct overlay_source*)data;

    replace_browser(context, nullptr);

    delete context;
}

static void overlay_source_update(void* data, obs_data_t* settings)
{
    struct overlay_source* context = (struct overlay_source*)data;
    
    // Update URL if changed
    const char* new_url = obs_data_get_string(settings, "url");
    if (new_url && context->url != new_url) {
        blog(LOG_INFO, "[VortiDeck Overlay] Applying refreshed browser-source capability");
        context->url = new_url;
        
        if (context->browser_source) {
            obs_data_t* browser_settings = obs_data_create();
            obs_data_set_string(browser_settings, "url", new_url);
            obs_source_update(context->browser_source, browser_settings);
            obs_data_release(browser_settings);
            blog(LOG_INFO, "[VortiDeck Overlay] Browser-source capability refreshed");
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
    
    context->auto_resize_enabled = auto_resize;
    // Update dimensions
    if (auto_resize) {
        // If auto-resize enabled, immediately resize to canvas
        handle_canvas_resize(context);
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
    
    std::string default_url = get_global_overlay_capability_url();
    if (default_url.empty()) {
        default_url = "about:blank";
        blog(LOG_INFO, "[VortiDeck Overlay] Waiting for an authenticated overlay capability");
    } else {
        blog(LOG_INFO, "[VortiDeck Overlay] Using the current authenticated overlay capability");
    }
    
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
    obs_source_t* browser = acquire_browser(context);
    const uint32_t width = browser ? obs_source_get_width(browser) : 0;
    obs_source_release(browser);
    return width;
}

static uint32_t overlay_source_get_height(void* data)
{
    struct overlay_source* context = (struct overlay_source*)data;
    obs_source_t* browser = acquire_browser(context);
    const uint32_t height = browser ? obs_source_get_height(browser) : 0;
    obs_source_release(browser);
    return height;
}

static void overlay_source_render(void* data, gs_effect_t* effect)
{
    UNUSED_PARAMETER(effect);
    // The editor's live preview shows the scene without VortiDeck's overlay;
    // the stream and recording (main mix) still draw it.
    if (vortideck::preview_feed::should_skip_overlay_render()) return;
    struct overlay_source* context = (struct overlay_source*)data;
    obs_source_t* browser = acquire_browser(context);
    if (browser) {
        obs_source_video_render(browser);
        obs_source_release(browser);
    }
}

static void overlay_source_enum_active_sources(void* data, obs_source_enum_proc_t enum_callback, void* param)
{
    struct overlay_source* context = (struct overlay_source*)data;
    obs_source_t* browser = acquire_browser(context);
    if (browser) {
        enum_callback(context->source, browser, param);
        obs_source_release(browser);
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

void synchronize_overlay_sources_to_canvas()
{
    obs_video_info video{};
    if (!obs_get_video_info(&video)) return;

    // Collect first, update afterwards. obs_enum_sources holds OBS's source
    // list lock; updating (which recreates the browser child) inside it
    // deadlocks against the graphics thread, which needs that lock to tick.
    struct collection { obs_video_info video; std::vector<obs_source_t*> sources; } pending{video, {}};
    obs_enum_sources([](void* data, obs_source_t* source) {
        auto& pending = *static_cast<collection*>(data);
        const char* source_id = obs_source_get_unversioned_id(source);
        if (!source_id || strcmp(source_id, vortideck::SOURCE_ID_OVERLAY) != 0) return true;
        obs_data_t* settings = obs_source_get_settings(source);
        const bool main = strcmp(obs_data_get_string(settings, "overlay_id"), "main_overlay") == 0;
        if ((main || obs_data_get_bool(settings, "auto_resize")) &&
            (obs_data_get_int(settings, "width") != pending.video.base_width ||
             obs_data_get_int(settings, "height") != pending.video.base_height ||
             obs_source_get_width(source) != pending.video.base_width ||
             obs_source_get_height(source) != pending.video.base_height)) {
            if (obs_source_t* ref = obs_source_get_ref(source)) pending.sources.push_back(ref);
        }
        obs_data_release(settings);
        return true;
    }, &pending);

    for (obs_source_t* source : pending.sources) {
        obs_data_t* settings = obs_source_get_settings(source);
        obs_data_set_int(settings, "width", video.base_width);
        obs_data_set_int(settings, "height", video.base_height);
        obs_source_update(source, settings);
        obs_data_release(settings);
        obs_source_release(source);
    }
}

vortideck_overlay_viewport get_main_overlay_viewport(uint32_t canvas_width, uint32_t canvas_height)
{
    struct observation { vortideck_overlay_viewport viewport; uint32_t width; uint32_t height; } result{{}, canvas_width, canvas_height};
    obs_enum_sources([](void* data, obs_source_t* source) {
        auto& result = *static_cast<observation*>(data);
        const char* source_id = obs_source_get_unversioned_id(source);
        if (!source_id || strcmp(source_id, vortideck::SOURCE_ID_OVERLAY) != 0) return true;
        obs_data_t* settings = obs_source_get_settings(source);
        const bool main = strcmp(obs_data_get_string(settings, "overlay_id"), "main_overlay") == 0;
        obs_data_release(settings);
        if (main) result.viewport.observe(obs_source_get_width(source), obs_source_get_height(source), result.width, result.height);
        return true;
    }, &result);
    return result.viewport;
}
