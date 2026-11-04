#pragma once
#include <cstdint>

void register_overlay_source();
void synchronize_overlay_sources_to_canvas();

struct vortideck_overlay_viewport {
    bool found = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t count = 0;
    uint32_t matching_count = 0;
    void observe(uint32_t source_width, uint32_t source_height, uint32_t canvas_width, uint32_t canvas_height) {
        if (!found) { width = source_width; height = source_height; }
        found = true;
        ++count;
        if (source_width == canvas_width && source_height == canvas_height) ++matching_count;
    }
    bool matches() const { return found && count == matching_count; }
};

vortideck_overlay_viewport get_main_overlay_viewport(uint32_t canvas_width, uint32_t canvas_height);
