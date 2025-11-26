// Simple compilation test for banner functionality
#include "obs_plugin/src/obs_plugin.hpp"

// Mock OBS functions for compilation test
typedef struct obs_source obs_source_t;
typedef struct obs_scene obs_scene_t;
typedef struct obs_sceneitem obs_sceneitem_t;
typedef struct obs_data obs_data_t;
typedef struct vec2 { float x, y; } vec2;

obs_source_t* obs_frontend_get_current_scene() { return nullptr; }
obs_scene_t* obs_scene_from_source(obs_source_t* source) { return nullptr; }
obs_sceneitem_t* obs_scene_find_source(obs_scene_t* scene, const char* name) { return nullptr; }
obs_sceneitem_t* obs_scene_add(obs_scene_t* scene, obs_source_t* source) { return nullptr; }
void obs_sceneitem_set_visible(obs_sceneitem_t* item, bool visible) {}
void obs_sceneitem_set_locked(obs_sceneitem_t* item, bool locked) {}
void obs_sceneitem_set_pos(obs_sceneitem_t* item, const struct vec2* pos) {}
void obs_sceneitem_set_scale(obs_sceneitem_t* item, const struct vec2* scale) {}
void obs_source_release(obs_source_t* source) {}
obs_data_t* obs_data_create() { return nullptr; }
void obs_data_release(obs_data_t* data) {}
void obs_data_set_string(obs_data_t* data, const char* key, const char* value) {}
void obs_data_set_bool(obs_data_t* data, const char* key, bool value) {}
obs_source_t* obs_source_create(const char* type, const char* name, obs_data_t* settings, obs_data_t* hotkey) { return nullptr; }
void obs_frontend_add_tools_menu_item(const char* name, void (*callback)(void*), void* data) {}
void blog(int level, const char* msg, ...) {}

int main() {
    // Test that header compiles
    return 0;
}