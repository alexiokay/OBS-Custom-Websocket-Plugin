#pragma once
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/matrix4.h>
#include <cmath>
#include <QUrl>
#include <QUuid>
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
#include <vector>
#include <memory>

// All entry points run on the OBS frontend thread. Data lives on the source so
// a scene rename or application restart does not change placement identity.
namespace vortideck::sponsor {
inline std::string bound_id;
struct items { std::vector<obs_sceneitem_t*> bound, legacy; std::string id; };

inline bool collect(obs_scene_t*, obs_sceneitem_t* item, void* opaque)
{
    auto& result = *static_cast<items*>(opaque);
    auto* source = obs_sceneitem_get_source(item);
    auto* settings = obs_source_get_settings(source);
    std::string binding = obs_data_get_string(settings, "vd_sponsor_placement");
    const std::string type = obs_source_get_unversioned_id(source);
    QUrl url(QString::fromUtf8(obs_data_get_string(settings, "url")));
    obs_data_release(settings);
    if (binding == result.id) result.bound.push_back(item);
    else if (binding.empty() && (type == "vortideck_banner_menu" ||
             (type == "browser_source" && url.path() == "/banners")))
        result.legacy.push_back(item);
    return true;
}

inline void bind(const std::string& id, const std::string& raw_url)
{
    const QUrl url(QString::fromStdString(raw_url));
    if ((url.scheme() != "http" && url.scheme() != "https") || url.path() != "/sponsor-player" ||
        url.host().isEmpty() || url.fragment().isEmpty()) throw std::runtime_error("Invalid sponsor player URL");
    obs_source_t* scene_source = obs_frontend_get_current_scene();
    if (!scene_source) throw std::runtime_error("Choose an OBS program scene first");
    std::unique_ptr<obs_source_t, decltype(&obs_source_release)> scene_ref(scene_source, obs_source_release);
    auto* scene = obs_scene_from_source(scene_source);
    if (!scene) throw std::runtime_error("Program target is not a scene");
    items found{{},{},id};
    obs_scene_enum_items(scene, collect, &found);
    if (found.bound.size() > 1 || (found.bound.empty() && found.legacy.size() > 1))
        throw std::runtime_error("Choose a scene with a single sponsor placement");
    // All scenes reuse one browser connection. Each opted-in scene item keeps its own transform.
    struct Search { std::string id; obs_source_t* source = nullptr; } search{id};
    obs_enum_sources([](void* data, obs_source_t* source) {
        auto& search = *static_cast<Search*>(data);
        auto* settings = obs_source_get_settings(source);
        const bool matches = search.id == obs_data_get_string(settings,"vd_sponsor_placement");
        obs_data_release(settings);
        if (matches) search.source = obs_source_get_ref(source);
        return !matches;
    }, &search);
    std::unique_ptr<obs_source_t, decltype(&obs_source_release)> shared(search.source, obs_source_release);
    if (shared && found.bound.empty() && !found.legacy.empty())
        throw std::runtime_error("Remove the extra legacy banner from this scene before connecting the existing sponsor player.");
    auto* config = obs_data_create();
    obs_data_set_string(config, "url", raw_url.c_str());
    obs_data_set_string(config, "vd_sponsor_placement", id.c_str());
    obs_data_set_bool(config, "is_local_file", false);
    obs_data_set_bool(config, "restart_when_active", false);
    obs_data_set_bool(config, "shutdown", false);
    obs_sceneitem_t* item = !found.bound.empty() ? found.bound.front() :
                             !found.legacy.empty() ? found.legacy.front() : nullptr;
    if (!item && shared) item = obs_scene_add(scene,shared.get());
    if (!item) {
        obs_video_info video{};
        obs_get_video_info(&video);
        obs_data_set_int(config, "width", video.base_width ? video.base_width : 1920);
        obs_data_set_int(config, "height", video.base_height ? video.base_height : 1080);
        auto* source = obs_source_create("vortideck_banner_menu", "VortiDeck Sponsor Banner", config, nullptr);
        if (source) {
            item = obs_scene_add(scene, source);
            obs_source_release(source);
        }
    }
    if (!item) { obs_data_release(config); throw std::runtime_error("Could not create sponsor source"); }
    auto* local = obs_sceneitem_get_private_settings(item);
    obs_data_set_string(local,"vd_sponsor_placement",id.c_str());
    obs_data_release(local);
    obs_source_update(obs_sceneitem_get_source(item),config);
    obs_data_release(config);
}

// OBS may load the saved URL before the desktop HTTP server exists. Reload
// only a previously opted-in source when its desktop session reconnects.
inline void reconnect(const std::string& id, const std::string& raw_url)
{
    const QUrl url(QString::fromStdString(raw_url));
    if (url.scheme() != "http" || url.host().isEmpty() ||
        url.path() != "/sponsor-player" || url.fragment().isEmpty()) return;
    struct Session { const std::string& id; const std::string& url; } session{id, raw_url};
    obs_enum_sources([](void* data, obs_source_t* source) {
        auto& session = *static_cast<Session*>(data);
        auto* config = obs_source_get_settings(source);
        const bool bound = session.id == obs_data_get_string(config, "vd_sponsor_placement");
        if (bound) {
            obs_data_set_string(config, "url", session.url.c_str());
            obs_source_update(source, config);
        }
        obs_data_release(config);
        return true;
    }, &session);
}

inline bool approved_item(obs_sceneitem_t* item, obs_data_t* config, const char* scene)
{
    auto* local = obs_sceneitem_get_private_settings(item);
    bool approved = bound_id == obs_data_get_string(local,"vd_sponsor_placement");
    obs_data_release(local);
    return approved || (std::string(scene) == obs_data_get_string(config,"vd_sponsor_scene") &&
        obs_sceneitem_get_id(item) == obs_data_get_int(config,"vd_sponsor_item"));
}

inline nlohmann::json frame(obs_sceneitem_t* item)
{
    obs_video_info video{};
    if (!obs_get_video_info(&video) || !video.base_width || !video.base_height) return nullptr;
    matrix4 box{};
    obs_sceneitem_get_box_transform(item, &box);
    obs_sceneitem_crop crop{};
    obs_sceneitem_get_crop(item, &crop);
    const bool editable = !obs_sceneitem_locked(item) && !crop.left && !crop.right && !crop.top && !crop.bottom &&
        std::abs(obs_sceneitem_get_rot(item)) < 0.001 && box.x.x > 0 && box.y.y > 0;
    auto* local = obs_sceneitem_get_private_settings(item);
    const bool fitted = obs_data_get_bool(local,"vd_sponsor_fitted");
    obs_data_release(local);
    return {{"canvasWidth",video.base_width},{"canvasHeight",video.base_height},{"x",box.t.x/video.base_width},{"y",box.t.y/video.base_height},
        {"width",box.x.x/video.base_width},{"height",box.y.y/video.base_height},
        {"editable",editable},{"fitted",fitted}};
}

inline void set_frame(const nlohmann::json& request)
{
    obs_source_t* current = obs_frontend_get_current_scene();
    std::unique_ptr<obs_source_t, decltype(&obs_source_release)> scene_ref(current, obs_source_release);
    if (!current || request.at("scene") != obs_source_get_uuid(current))
        throw std::runtime_error("The program scene changed. Reopen banner placement.");
    items found{{},{},bound_id};
    obs_scene_enum_items(obs_scene_from_source(current), collect, &found);
    if (found.bound.size() != 1) throw std::runtime_error("Select the scene containing the bound banner.");
    auto* item = found.bound.front();
    auto* source = obs_sceneitem_get_source(item);
    std::unique_ptr<obs_data_t, decltype(&obs_data_release)> config(obs_source_get_settings(source), obs_data_release);
    if (request.at("source") != obs_source_get_uuid(source) || request.at("item") != obs_sceneitem_get_id(item) ||
        !approved_item(item, config.get(), obs_source_get_uuid(current)))
        throw std::runtime_error("The banner source changed. Reopen banner placement.");
    const auto current_frame = frame(item);
    if (current_frame.is_null() || !current_frame.at("editable").get<bool>())
        throw std::runtime_error("Unlock the source and clear crop, rotation or mirroring in OBS to edit its frame.");
    for (const auto* k : {"x","y","width","height"}) {
        if (std::abs(current_frame.at(k).get<double>() - request.at("original").at(k).get<double>()) > 0.0001)
            throw std::runtime_error("The banner moved in OBS. Reload placement before applying.");
    }
    auto rect = request.at("frame");
    const double x=rect.at("x"), y=rect.at("y"), w=rect.at("width"), h=rect.at("height");
    if (!(x>=0 && y>=0 && w>=0.01 && h>=0.01 && x+w<=1.000001 && y+h<=1.000001))
        throw std::runtime_error("Keep the banner inside the canvas.");
    obs_video_info video{};
    if (!obs_get_video_info(&video)) throw std::runtime_error("OBS canvas is unavailable.");
    vec2 pos{}, bounds{};
    vec2_set(&pos, x*video.base_width, y*video.base_height);
    vec2_set(&bounds, w*video.base_width, h*video.base_height);
    auto* local = obs_sceneitem_get_private_settings(item);
    obs_data_set_bool(local,"vd_sponsor_fitted",true);
    obs_data_release(local);
    obs_sceneitem_defer_update_begin(item);
    obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
    obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
    obs_sceneitem_set_bounds(item,&bounds);
    obs_sceneitem_set_pos(item,&pos);
    obs_sceneitem_defer_update_end(item);
    obs_frontend_save();
}

inline nlohmann::json observe()
{
    nlohmann::json observation = {{"type","SPONSOR_OBSERVATION"},{"placement",bound_id},
        {"streaming",obs_frontend_streaming_active()},{"recording",obs_frontend_recording_active()},
        {"visible",false}};
    auto* scene_source = obs_frontend_get_current_scene();
    if (!scene_source) return observation;
    auto* scene = obs_scene_from_source(scene_source);
    items found{{},{},bound_id};
    if (scene) obs_scene_enum_items(scene, collect, &found);
    for (auto* item : found.bound) {
        auto* source = obs_sceneitem_get_source(item);
        auto* config = obs_source_get_settings(source);
        const std::string scene_id = obs_source_get_uuid(scene_source);
        const auto item_id = obs_sceneitem_get_id(item);
        if (approved_item(item,config,scene_id.c_str()) && found.bound.size() == 1) {
            observation["visible"] = obs_sceneitem_visible(item);
            observation["frame"] = frame(item);
            observation["scene"] = scene_id;
            observation["item"] = item_id;
            observation["source"] = obs_source_get_uuid(source);
        }
        obs_data_release(config);
    }
    obs_source_release(scene_source);
    return observation;
}
}
