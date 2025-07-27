#pragma once

#include <obs.h>
#include <obs-module.h>
#include <string>

namespace vortideck {

enum class SourceType {
    ADS,
    OVERLAY
};

const char* const SOURCE_ID_ADS = "vortideck_ads";
const char* const SOURCE_ID_OVERLAY = "vortideck_overlay";

const char* const META_TYPE = "vortideck_type";
const char* const META_ID = "vortideck_id";
const char* const META_BANNER_ID = "vortideck_banner_id";
const char* const META_BANNER_TYPE = "vortideck_banner_type";

inline SourceType get_source_type(obs_source_t* source) {
    if (!source) return SourceType::ADS;
    
    obs_data_t* settings = obs_source_get_private_settings(source);
    const char* type = obs_data_get_string(settings, META_TYPE);
    obs_data_release(settings);
    
    if (strcmp(type, "overlay") == 0) {
        return SourceType::OVERLAY;
    }
    return SourceType::ADS;
}

inline void set_source_type(obs_source_t* source, SourceType type) {
    if (!source) return;
    
    obs_data_t* settings = obs_source_get_private_settings(source);
    obs_data_set_string(settings, META_TYPE, type == SourceType::OVERLAY ? "overlay" : "ads");
    obs_data_release(settings);
}

inline bool is_vortideck_source(obs_source_t* source) {
    if (!source) return false;
    
    const char* id = obs_source_get_id(source);
    return (strcmp(id, SOURCE_ID_ADS) == 0 || strcmp(id, SOURCE_ID_OVERLAY) == 0);
}

} // namespace vortideck