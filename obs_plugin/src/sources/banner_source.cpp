#include "banner_source.h"
#include <obs-module.h>

namespace {
struct banner_source {
    obs_source_t* source;
    obs_source_t* browser_source;
};

const char* banner_source_get_name(void*) { return "VortiDeck Sponsor Banner"; }

obs_data_t* browser_settings(obs_data_t* settings)
{
    obs_data_t* browser = obs_data_create();
    const char* url = obs_data_get_string(settings, "url");
    obs_data_set_string(browser, "url", url && *url ? url : "about:blank");
    obs_data_set_bool(browser, "is_local_file", false);
    obs_data_set_bool(browser, "restart_when_active", false);
    obs_data_set_bool(browser, "shutdown", false);
    obs_data_set_bool(browser, "reroute_audio", true);
    obs_data_set_int(browser, "width", obs_data_get_int(settings, "width"));
    obs_data_set_int(browser, "height", obs_data_get_int(settings, "height"));
    return browser;
}

void* banner_source_create(obs_data_t* settings, obs_source_t* source)
{
    obs_data_t* config = browser_settings(settings);
    auto* child = obs_source_create_private("browser_source", "Sponsor player", config);
    obs_data_release(config);
    if (!child) return nullptr;
    obs_source_add_active_child(source, child);
    return new banner_source{source, child};
}

void banner_source_destroy(void* data)
{
    auto* context = static_cast<banner_source*>(data);
    obs_source_remove_active_child(context->source, context->browser_source);
    obs_source_release(context->browser_source);
    delete context;
}

void banner_source_update(void* data, obs_data_t* settings)
{
    auto* context = static_cast<banner_source*>(data);
    auto* config = browser_settings(settings);
    obs_source_update(context->browser_source, config);
    obs_data_release(config);
}

void banner_source_defaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "url", "about:blank");
    // Preserve the legacy wrapper's dimensions for existing placements.
    obs_data_set_default_int(settings, "width", 1920);
    obs_data_set_default_int(settings, "height", 100);
}

obs_properties_t* banner_source_properties(void*)
{
    auto* props = obs_properties_create();
    obs_properties_add_text(props, "url", "Sponsor player URL", OBS_TEXT_PASSWORD);
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

}

void register_banner_source()
{
    struct obs_source_info banner_info = {};
    banner_info.id = "vortideck_banner_menu";
    banner_info.type = OBS_SOURCE_TYPE_INPUT;
    banner_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
    banner_info.get_name = banner_source_get_name;
    banner_info.create = banner_source_create;
    banner_info.destroy = banner_source_destroy;
    banner_info.update = banner_source_update;
    banner_info.get_defaults = banner_source_defaults;
    banner_info.get_properties = banner_source_properties;
    banner_info.get_width = banner_source_get_width;
    banner_info.get_height = banner_source_get_height;
    banner_info.video_render = banner_source_render;
    banner_info.enum_active_sources = [](void* data, obs_source_enum_proc_t callback, void* param) {
        auto* context = static_cast<banner_source*>(data);
        callback(context->source, context->browser_source, param);
    };
    
    obs_register_source(&banner_info);
    blog(LOG_INFO, "VortiDeck Banner: Registered creator-controlled sponsor source");
}
