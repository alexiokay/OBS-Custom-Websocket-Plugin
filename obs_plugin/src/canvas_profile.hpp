#pragma once
#include <util/config-file.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

struct canvas_size { uint32_t width; uint32_t height; };

// The scaled (encoded) output that matches a new base canvas. It keeps the
// user's output quality (its short edge) but follows the new base aspect, so
// switching to portrait does not squash a 1080x1920 canvas into 1920x1080.
// A canvas streamed without downscaling stays unscaled. Sizes are even
// because 4:2:0 encoders reject odd output dimensions.
inline canvas_size scaled_output_for_canvas(canvas_size old_base, canvas_size old_output, canvas_size new_base)
{
    auto even = [](double value) {
        const auto rounded = static_cast<uint32_t>(std::lround(value));
        return std::max<uint32_t>(2, rounded & ~1u);
    };
    double scale = 1.0;
    if (old_output.width != old_base.width || old_output.height != old_base.height) {
        const double short_edge = static_cast<double>(std::min(old_output.width, old_output.height));
        scale = std::min(1.0, short_edge / static_cast<double>(std::min(new_base.width, new_base.height)));
    }
    return {even(new_base.width * scale), even(new_base.height * scale)};
}

namespace canvas_profile_detail {
struct saved_value { const char* key; uint64_t value; bool present; };

inline saved_value remember(config_t* profile, const char* key)
{
    return {key, config_get_uint(profile, "Video", key), config_has_user_value(profile, "Video", key)};
}

inline void restore(config_t* profile, const saved_value& saved)
{
    if (saved.present) config_set_uint(profile, "Video", saved.key, saved.value);
    else config_remove_value(profile, "Video", saved.key);
}
} // namespace canvas_profile_detail

// Persist the base canvas and, when given, the scaled output. On a failed
// write the in-memory profile is restored so it keeps describing what OBS uses.
inline bool save_canvas_profile(config_t* profile, canvas_size base, const canvas_size* output = nullptr)
{
    using namespace canvas_profile_detail;
    if (!profile) return false;
    const saved_value saved[] = {
        remember(profile, "BaseCX"), remember(profile, "BaseCY"),
        remember(profile, "OutputCX"), remember(profile, "OutputCY"),
    };
    config_set_uint(profile, "Video", "BaseCX", base.width);
    config_set_uint(profile, "Video", "BaseCY", base.height);
    if (output) {
        config_set_uint(profile, "Video", "OutputCX", output->width);
        config_set_uint(profile, "Video", "OutputCY", output->height);
    }
    if (config_save_safe(profile, "tmp", "bak") == CONFIG_SUCCESS) return true;
    for (const auto& value : saved) restore(profile, value);
    return false;
}

// Base canvas only; the encoded output size stays as OBS has it.
inline bool save_canvas_profile(config_t* profile, uint32_t width, uint32_t height)
{
    return save_canvas_profile(profile, canvas_size{width, height});
}
