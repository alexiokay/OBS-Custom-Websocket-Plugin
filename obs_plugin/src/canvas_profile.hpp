#pragma once
#include <util/config-file.h>
#include <cstdint>

// Persist only the base canvas; encoded output size remains OBS-owned.
inline bool save_canvas_profile(config_t* profile, uint32_t width, uint32_t height)
{
    if (!profile) return false;
    const auto old_width = config_get_uint(profile, "Video", "BaseCX");
    const auto old_height = config_get_uint(profile, "Video", "BaseCY");
    const bool had_width = config_has_user_value(profile, "Video", "BaseCX");
    const bool had_height = config_has_user_value(profile, "Video", "BaseCY");
    config_set_uint(profile, "Video", "BaseCX", width);
    config_set_uint(profile, "Video", "BaseCY", height);
    if (config_save_safe(profile, "tmp", "bak") == CONFIG_SUCCESS) return true;
    if (had_width) config_set_uint(profile, "Video", "BaseCX", old_width);
    else config_remove_value(profile, "Video", "BaseCX");
    if (had_height) config_set_uint(profile, "Video", "BaseCY", old_height);
    else config_remove_value(profile, "Video", "BaseCY");
    return false;
}
