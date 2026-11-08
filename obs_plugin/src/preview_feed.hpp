#pragma once
// Live OBS preview feed for the VortiDeck overlay editor.
//
// Renders the program output without VortiDeck overlay sources into a private
// view at the base canvas size, encodes it with a hardware H.264 encoder
// (x264 fallback) and streams it over its own websocket connection. Completely
// idle until VortiDeck sends PREVIEW_FEED_START. A stall or failure here never
// touches the control connection. See obs_plugin/docs/PREVIEW_FEED_PLAN.md.

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace vortideck::preview_feed {

struct control_endpoint {
    bool secure = false;
    std::string host;
    uint16_t port = 0;
};

// Sends a JSON message over the (authenticated) control connection.
using control_sender = std::function<bool(const nlohmann::json&)>;

// Registers the private source and output types. Call once from
// obs_module_post_load on the OBS UI thread.
void initialize(control_sender sender);

// Capability advertised in the companion registration.
nlohmann::json capability();

// True while the preview view renders; VortiDeck overlay sources must draw
// nothing then. Counts the skip so the editor can tell whether an overlay was
// actually hidden. Graphics thread only.
bool should_skip_overlay_render();

// PREVIEW_FEED_START / _KEEPALIVE / _STOP. Any thread; returns true when the
// message type belonged to the preview feed.
bool handle_control_message(const nlohmann::json& message, const control_endpoint& endpoint);

// The control connection closed: stop streaming. Any thread.
void on_control_disconnected();

// Called every second from the canvas observer (OBS UI thread): follows canvas
// size changes, enforces the keepalive and the start watchdog.
void tick();

// Canvas Apply needs OBS video to be inactive. UI thread; suspend blocks until
// the preview encoder has stopped (bounded).
void suspend_for_video_reset();
void resume_after_video_reset();

// OBS is exiting. UI thread.
void shutdown();

} // namespace vortideck::preview_feed
