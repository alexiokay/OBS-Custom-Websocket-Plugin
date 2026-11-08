// Live OBS preview feed. See preview_feed.hpp and docs/PREVIEW_FEED_PLAN.md.
//
// Threads:
// - OBS UI thread: all feed state, and every libobs object (view, encoder,
//   output) is created and destroyed here. Other threads only post to it.
// - Graphics thread: renders the private root source (overlay skip flag).
// - Encoder thread: encoded_packet copies into the session queue, never blocks.
// - Preview socket thread: one websocketpp client with its own asio loop,
//   separate from the control connection.

#define _WEBSOCKETPP_CPP11_RANDOM_DEVICE_
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#ifndef _WEBSOCKETPP_CPP11_TYPE_TRAITS_
#define _WEBSOCKETPP_CPP11_TYPE_TRAITS_
#endif

#pragma warning(push)
#pragma warning(disable : 4267)
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#pragma warning(pop)

#include "preview_feed.hpp"
#include "preview_feed_protocol.hpp"

#include <obs.h>
#include <obs-output.h>
#include <obs-source.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QMetaObject>

#include <atomic>
#include <cctype>
#include <cmath>
#include <format>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vortideck::preview_feed {
namespace {

namespace proto = vortideck::preview;
using ws_client = websocketpp::client<websocketpp::config::asio_client>;
using clock = std::chrono::steady_clock;

constexpr const char* k_root_source_id = "vortideck_preview_root";
constexpr const char* k_output_id = "vortideck_preview_output";
constexpr size_t k_max_buffered_bytes = 2 * 1024 * 1024;
constexpr auto k_start_watchdog = std::chrono::seconds(4);

thread_local bool t_preview_pass = false;
std::atomic<uint64_t> g_overlay_skips{0};
std::atomic<bool> g_shutting_down{false};
control_sender g_send;
bool g_initialized = false;

void log(int level, const std::string& message) { blog(level, "[VortiDeck Preview] %s", message.c_str()); }

template <typename F> void on_ui(F&& fn) {
    if (!qApp || g_shutting_down) return;
    QMetaObject::invokeMethod(qApp, std::forward<F>(fn), Qt::QueuedConnection);
}

int64_t now_us() { return static_cast<int64_t>(os_gettime_ns() / 1000); }

// ---------------------------------------------------------------------------
// Preview socket session: own thread, own asio loop.
// ---------------------------------------------------------------------------
struct socket_session {
    uint64_t generation = 0;
    ws_client client;
    std::thread thread;
    websocketpp::connection_hdl hdl;
    std::atomic<bool> ready{false};
    std::atomic<bool> closing{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> drain_posted{false};
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> sent{0};

    std::mutex mutex; // queue, sequence, size, extra data
    proto::frame_queue queue{8};
    uint32_t sequence = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    const void* extra_encoder = nullptr;
    std::vector<uint8_t> extra;

    ~socket_session() {
        if (thread.joinable()) thread.detach(); // shutdown() joins; never terminate here
    }

    void set_frame_size(uint32_t w, uint32_t h) {
        std::lock_guard lock(mutex);
        width = static_cast<uint16_t>(w);
        height = static_cast<uint16_t>(h);
        queue.clear();
        extra_encoder = nullptr;
        extra.clear();
    }

    void clear_queue() {
        std::lock_guard lock(mutex);
        queue.clear();
    }

    // Encoder thread.
    void enqueue(const encoder_packet* packet) {
        if (closing || !ready || !packet || packet->type != OBS_ENCODER_VIDEO || !packet->data || !packet->size) return;
        packets++;
        {
            std::lock_guard lock(mutex);
            if (packet->keyframe && packet->encoder && packet->encoder != extra_encoder) {
                uint8_t* data = nullptr;
                size_t size = 0;
                extra.clear();
                if (obs_encoder_get_extra_data(packet->encoder, &data, &size) && data && size) extra.assign(data, data + size);
                extra_encoder = packet->encoder;
            }
            const bool with_config = packet->keyframe && !extra.empty();
            proto::queued_frame frame;
            frame.keyframe = packet->keyframe;
            frame.bytes.resize(proto::k_header_size + (with_config ? extra.size() : 0) + packet->size);
            proto::frame_header header;
            header.flags = static_cast<uint8_t>((packet->keyframe ? proto::k_flag_keyframe : 0) |
                                                (with_config ? proto::k_flag_config : 0));
            header.sequence = ++sequence;
            header.width = width;
            header.height = height;
            if (packet->timebase_den > 0)
                header.pts_us = packet->pts * 1000000LL * packet->timebase_num / packet->timebase_den;
            header.render_time_us = packet->sys_dts_usec;
            proto::write_header(frame.bytes.data(), header);
            uint8_t* out = frame.bytes.data() + proto::k_header_size;
            if (with_config) {
                std::memcpy(out, extra.data(), extra.size());
                out += extra.size();
            }
            std::memcpy(out, packet->data, packet->size);
            queue.push(std::move(frame));
        }
        if (!drain_posted.exchange(true)) {
            try {
                asio::post(client.get_io_service(), [this] { drain(); });
            } catch (...) {
                drain_posted = false;
            }
        }
    }

    // Socket thread.
    void drain() {
        drain_posted = false;
        if (closing || !ready) return;
        websocketpp::lib::error_code ec;
        auto connection = client.get_con_from_hdl(hdl, ec);
        if (ec || !connection) return;
        for (;;) {
            std::optional<proto::queued_frame> frame;
            {
                std::lock_guard lock(mutex);
                if (connection->get_buffered_amount() > k_max_buffered_bytes) {
                    queue.drop_to_next_keyframe(); // the network is behind; resync on a keyframe
                    return;
                }
                frame = queue.pop();
            }
            if (!frame) return;
            const int64_t send_time = now_us();
            for (size_t i = 0; i < 8; ++i) frame->bytes[32 + i] = static_cast<uint8_t>(static_cast<uint64_t>(send_time) >> (8 * i));
            ec = connection->send(frame->bytes.data(), frame->bytes.size(), websocketpp::frame::opcode::binary);
            if (ec) return;
            sent++;
        }
    }

    // UI thread. Bounded: a stalled peer cannot hold OBS's UI thread.
    void shutdown() {
        closing = true;
        if (!thread.joinable()) return;
        try {
            asio::post(client.get_io_service(), [this] {
                websocketpp::lib::error_code ec;
                client.close(hdl, websocketpp::close::status::going_away, "preview stopped", ec);
            });
        } catch (...) {
        }
        const auto deadline = clock::now() + std::chrono::milliseconds(300);
        while (!finished && clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!finished) {
            try {
                client.stop();
            } catch (...) {
            }
        }
        if (thread.joinable()) thread.join();
    }
};

std::mutex g_session_mutex;
std::shared_ptr<socket_session> g_session; // current session, read by the encoder thread

std::shared_ptr<socket_session> current_session() {
    std::lock_guard lock(g_session_mutex);
    return g_session;
}

// ---------------------------------------------------------------------------
// Feed state: OBS UI thread only.
// ---------------------------------------------------------------------------
enum class phase { idle, connecting, running, suspended };

struct pipeline {
    obs_source_t* root = nullptr;
    obs_view_t* view = nullptr;
    video_t* video = nullptr;
    obs_output_t* output = nullptr;
    obs_encoder_t* encoder = nullptr;
};

struct feed_state {
    phase current = phase::idle;
    uint64_t generation = 0;
    std::string request_id;
    std::string ticket;
    std::string path;
    uint32_t requested_fps = 30;
    control_endpoint endpoint;
    std::shared_ptr<socket_session> session;
    pipeline p;
    proto::keepalive keepalive;
    clock::time_point pipeline_started{};
    uint64_t packets_at_start = 0;
    bool watchdog_armed = false;
    std::vector<std::string> failed_encoders;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t obs_fps_num = 0;
    uint32_t obs_fps_den = 0;
    proto::fps_plan fps;
    uint32_t bitrate = 0;
    std::optional<proto::encoder_candidate> encoder;
    std::string encoder_name;
    std::string fallback_reason;
    uint64_t overlay_skips_seen = 0;
    bool overlay_hidden = false;
};
feed_state g_state;

void send_status(const char* state, const std::string& reason = {}) {
    if (!g_send) return;
    nlohmann::json encoder = nullptr;
    if (g_state.encoder) {
        encoder = {{"id", g_state.encoder->id},
                   {"name", g_state.encoder_name},
                   {"family", g_state.encoder->family},
                   {"hardware", g_state.encoder->hardware},
                   {"fallback_reason", g_state.fallback_reason}};
    }
    nlohmann::json status = {
        {"type", "PREVIEW_FEED_STATUS"},
        {"state", state},
        {"reason", reason},
        {"request_id", g_state.request_id},
        {"width", g_state.width},
        {"height", g_state.height},
        {"fps", g_state.fps.effective},
        {"requested_fps", g_state.requested_fps},
        {"fps_limited_by_obs", g_state.fps.limited_by_obs},
        {"bitrate_kbps", g_state.bitrate},
        {"keyint_sec", 1},
        {"encoder", encoder},
        {"overlay_hidden", g_state.overlay_hidden},
    };
    try {
        g_send(status);
    } catch (...) {
    }
}

// ---- private root source: the program output, rendered as a preview pass ----
const char* root_get_name(void*) { return "VortiDeck Preview Root"; }
void* root_create(obs_data_t*, obs_source_t* source) { return source; }
void root_destroy(void*) {}
uint32_t root_get_width(void*) {
    obs_video_info ovi{};
    return obs_get_video_info(&ovi) ? ovi.base_width : 0;
}
uint32_t root_get_height(void*) {
    obs_video_info ovi{};
    return obs_get_video_info(&ovi) ? ovi.base_height : 0;
}
void root_render(void*, gs_effect_t*) {
    obs_source_t* program = obs_get_output_source(0); // the program transition
    if (!program) return;
    {
        proto::render_pass_scope scope(t_preview_pass);
        obs_source_video_render(program);
    }
    obs_source_release(program);
}

// ---- private output: hands encoded packets to the socket session ----
const char* output_get_name(void*) { return "VortiDeck Preview Output"; }
void* output_create(obs_data_t*, obs_output_t* output) { return output; }
void output_destroy(void*) {}
bool output_start(void* data) {
    auto* output = static_cast<obs_output_t*>(data);
    if (!obs_output_can_begin_data_capture(output, 0)) return false;
    if (!obs_output_initialize_encoders(output, 0)) return false;
    return obs_output_begin_data_capture(output, 0);
}
void output_stop(void* data, uint64_t) { obs_output_end_data_capture(static_cast<obs_output_t*>(data)); }
void output_packet(void*, encoder_packet* packet) {
    if (auto session = current_session()) session->enqueue(packet);
}

// ---- encoder settings: only keys this encoder actually has, typed ----
void set_if_known(obs_data_t* data, const char* key, const char* value) {
    obs_data_item_t* item = obs_data_item_byname(data, key);
    if (!item) return;
    if (obs_data_item_gettype(item) == OBS_DATA_STRING) obs_data_set_string(data, key, value);
    obs_data_item_release(&item);
}
void set_if_known(obs_data_t* data, const char* key, long long value) {
    obs_data_item_t* item = obs_data_item_byname(data, key);
    if (!item) return;
    if (obs_data_item_gettype(item) == OBS_DATA_NUMBER) obs_data_set_int(data, key, value);
    obs_data_item_release(&item);
}
void set_if_known(obs_data_t* data, const char* key, bool value) {
    obs_data_item_t* item = obs_data_item_byname(data, key);
    if (!item) return;
    if (obs_data_item_gettype(item) == OBS_DATA_BOOLEAN) obs_data_set_bool(data, key, value);
    obs_data_item_release(&item);
}

obs_data_t* encoder_settings(const proto::encoder_candidate& candidate, uint32_t kbps) {
    obs_data_t* data = obs_encoder_defaults(candidate.id);
    if (!data) return obs_data_create();
    set_if_known(data, "rate_control", "CBR");
    set_if_known(data, "bitrate", static_cast<long long>(kbps));
    set_if_known(data, "keyint_sec", 1LL);
    set_if_known(data, "bf", 0LL);
    set_if_known(data, "bframes", 0LL);
    // Constrained Baseline: no frame reordering is possible, so hardware
    // decoders (WebView2/Chromium) output every frame at once instead of
    // holding a full DPB (measured: ~7 frames, 234 ms, with High profile).
    set_if_known(data, "profile", "baseline");
    const std::string family = candidate.family;
    if (family == "nvenc") {
        set_if_known(data, "preset", "p1");
        set_if_known(data, "preset2", "p1");
        set_if_known(data, "tune", "ull");
        set_if_known(data, "multipass", "disabled");
        set_if_known(data, "lookahead", false);
    } else if (family == "amf") {
        set_if_known(data, "preset", "speed");
    } else if (family == "qsv") {
        set_if_known(data, "target_usage", "TU7");
        set_if_known(data, "latency", "ultra-low");
    } else {
        set_if_known(data, "preset", "ultrafast");
        set_if_known(data, "tune", "zerolatency");
    }
    return data;
}

bool encoder_offered(const char* id) {
    const char* type = nullptr;
    for (size_t i = 0; obs_enum_encoder_types(i, &type); ++i) {
        if (type && std::strcmp(type, id) == 0) {
            const char* codec = obs_get_encoder_codec(id);
            return codec && std::strcmp(codec, "h264") == 0;
        }
    }
    return false;
}

// ---- pipeline lifecycle (UI thread) ----
// Long-lived objects: the private root source and the view live for the OBS
// session, and the encoder/output pair is kept while its settings still
// match. Only the video mix (obs_view_add2) is created per start and removed
// per stop, so nothing renders or encodes while the preview is off, and
// repeated toggling does not churn libobs objects.
struct cached_encoder {
    obs_encoder_t* encoder = nullptr;
    obs_output_t* output = nullptr;
    std::string key;
};
cached_encoder g_cached;

std::string encoder_key(const proto::encoder_candidate& candidate) {
    return std::format("{}|{}x{}|{}|{}", candidate.id, g_state.width, g_state.height, g_state.fps.divisor, g_state.bitrate);
}

void release_cached_encoder() {
    if (g_cached.output) obs_output_release(g_cached.output);
    if (g_cached.encoder) obs_encoder_release(g_cached.encoder);
    g_cached = {};
}

void teardown_pipeline() {
    auto& p = g_state.p;
    if (p.output) {
        obs_output_stop(p.output);
        const auto deadline = clock::now() + std::chrono::seconds(2);
        while (obs_output_active(p.output) && clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (p.view && p.video) obs_view_remove(p.view); // libobs frees the mix on its next output pass
    p.video = nullptr;
    p.output = nullptr; // owned by g_cached
    p.encoder = nullptr;
    g_state.watchdog_armed = false;
    if (g_state.session) g_state.session->clear_queue();
}

void release_pipeline_objects() {
    teardown_pipeline();
    release_cached_encoder();
    auto& p = g_state.p;
    if (p.view) {
        obs_view_set_source(p.view, 0, nullptr);
        obs_view_destroy(p.view);
    }
    if (p.root) obs_source_release(p.root);
    p = {};
}

void stop_all(const char* state, const std::string& reason, bool notify) {
    const bool was_active = g_state.current != phase::idle;
    teardown_pipeline();
    std::shared_ptr<socket_session> session;
    {
        std::lock_guard lock(g_session_mutex);
        session = std::move(g_session);
    }
    if (session) session->shutdown();
    g_state.session.reset();
    g_state.current = phase::idle;
    g_state.ticket.clear();
    if (notify && was_active) send_status(state, reason);
    if (was_active) log(LOG_INFO, std::string("stopped") + (reason.empty() ? "" : ": " + reason));
}

std::string try_encoder(const proto::encoder_candidate& candidate) {
    auto& p = g_state.p;
    const std::string key = encoder_key(candidate);
    if (g_cached.encoder && g_cached.key == key) {
        obs_encoder_set_video(g_cached.encoder, p.video); // the previous mix is gone
        if (obs_output_start(g_cached.output)) {
            p.output = g_cached.output;
            p.encoder = g_cached.encoder;
            return {};
        }
        const char* error = obs_output_get_last_error(g_cached.output);
        std::string reason = error && *error ? error : "did not start";
        release_cached_encoder();
        return reason;
    }
    release_cached_encoder(); // different encoder, size, rate or bitrate

    obs_data_t* settings = encoder_settings(candidate, g_state.bitrate);
    obs_encoder_t* encoder = obs_video_encoder_create(candidate.id, "VortiDeck Preview Encoder", settings, nullptr);
    obs_data_release(settings);
    if (!encoder) return "could not be created";
    obs_encoder_set_video(encoder, p.video);
    if (g_state.fps.divisor > 1 && !obs_encoder_set_frame_rate_divisor(encoder, g_state.fps.divisor)) {
        obs_encoder_release(encoder);
        return "frame-rate divisor rejected";
    }
    obs_output_t* output = obs_output_create(k_output_id, "VortiDeck Preview Output", nullptr, nullptr);
    if (!output) {
        obs_encoder_release(encoder);
        return "preview output unavailable";
    }
    obs_output_set_video_encoder(output, encoder);
    if (!obs_output_start(output)) {
        const char* error = obs_output_get_last_error(output);
        std::string reason = error && *error ? error : "did not start";
        obs_output_release(output);
        obs_encoder_release(encoder);
        return reason;
    }
    g_cached = {encoder, output, key};
    p.output = output;
    p.encoder = encoder;
    return {};
}

bool start_pipeline() {
    obs_video_info main{};
    if (!obs_get_video_info(&main)) {
        stop_all("failed", "OBS video is not initialised", true);
        return false;
    }
    g_state.width = main.base_width;
    g_state.height = main.base_height;
    g_state.obs_fps_num = main.fps_num;
    g_state.obs_fps_den = main.fps_den;
    g_state.fps = proto::plan_fps(main.fps_num, main.fps_den, g_state.requested_fps);
    g_state.bitrate = proto::bitrate_kbps(main.base_width, main.base_height,
                                          static_cast<uint32_t>(std::lround(g_state.fps.effective)));
    if (g_state.session) g_state.session->set_frame_size(main.base_width, main.base_height);

    auto& p = g_state.p;
    if (!p.root) p.root = obs_source_create_private(k_root_source_id, "VortiDeck Preview Root", nullptr);
    if (p.root && !p.view) {
        p.view = obs_view_create();
        if (p.view) obs_view_set_source(p.view, 0, p.root);
    }
    if (!p.root || !p.view) {
        teardown_pipeline();
        stop_all("failed", "Could not create the preview view", true);
        return false;
    }

    // The already-synced base canvas, 1:1. Hardware H.264 needs NV12/SDR.
    obs_video_info ovi = main;
    ovi.output_width = main.base_width;
    ovi.output_height = main.base_height;
    ovi.output_format = VIDEO_FORMAT_NV12;
    if (ovi.colorspace == VIDEO_CS_2100_PQ || ovi.colorspace == VIDEO_CS_2100_HLG) ovi.colorspace = VIDEO_CS_709;
    ovi.gpu_conversion = true;
    p.video = obs_view_add2(p.view, &ovi);
    if (!p.video) {
        teardown_pipeline();
        stop_all("failed", "OBS refused the preview view", true);
        return false;
    }

    auto candidates = proto::available_encoders([](const char* id) {
        for (const auto& failed : g_state.failed_encoders)
            if (failed == id) return false;
        // Diagnostics only: force one encoder (for example obs_x264) to compare.
        if (const char* forced = std::getenv("VORTIDECK_PREVIEW_ENCODER"); forced && *forced && std::strcmp(forced, id) != 0)
            return false;
        return encoder_offered(id);
    });
    auto choice = proto::choose_encoder(candidates, try_encoder);
    if (!choice.chosen) {
        teardown_pipeline();
        stop_all("failed", "No H.264 encoder could start: " + choice.failure, true);
        return false;
    }
    g_state.encoder = choice.chosen;
    const char* display = obs_encoder_get_display_name(choice.chosen->id);
    g_state.encoder_name = display ? display : choice.chosen->id;
    if (!choice.fallback_reason.empty() || g_state.fallback_reason.empty()) g_state.fallback_reason = choice.fallback_reason;
    g_state.pipeline_started = clock::now();
    g_state.packets_at_start = g_state.session ? g_state.session->packets.load() : 0;
    g_state.watchdog_armed = true;
    g_state.current = phase::running;
    log(LOG_INFO, std::format("running {}x{} at {:.2f} fps, {} kbps, encoder {}{}", g_state.width, g_state.height,
                              g_state.fps.effective, g_state.bitrate, choice.chosen->id,
                              g_state.fallback_reason.empty() ? "" : " (fallback: " + g_state.fallback_reason + ")"));
    send_status("running");
    return true;
}

// ---- socket callbacks, marshalled to the UI thread ----
void socket_ready(uint64_t generation) {
    if (generation != g_state.generation || g_state.current != phase::connecting) return;
    start_pipeline();
}

void socket_closed(uint64_t generation, const std::string& reason, bool failed) {
    if (generation != g_state.generation || g_state.current == phase::idle) return;
    stop_all(failed ? "failed" : "stopped", reason, true);
}

bool open_socket(const std::string& url) {
    auto session = std::make_shared<socket_session>();
    session->generation = g_state.generation;
    auto* raw = session.get();
    const uint64_t generation = g_state.generation;
    const std::string ticket = g_state.ticket;
    try {
        auto& client = session->client;
        client.clear_access_channels(websocketpp::log::alevel::all);
        client.clear_error_channels(websocketpp::log::elevel::all);
        client.init_asio();
        client.set_open_handler([raw, ticket](websocketpp::connection_hdl hdl) {
            websocketpp::lib::error_code ec;
            raw->client.send(hdl, nlohmann::json({{"type", "PREVIEW_HELLO"}, {"ticket", ticket}, {"version", proto::k_capability_version}}).dump(),
                             websocketpp::frame::opcode::text, ec);
        });
        client.set_message_handler([raw, generation](websocketpp::connection_hdl, ws_client::message_ptr message) {
            if (message->get_opcode() != websocketpp::frame::opcode::text) return;
            try {
                const auto json = nlohmann::json::parse(message->get_payload());
                if (json.value("type", "") == "PREVIEW_READY" && !raw->ready.exchange(true))
                    on_ui([generation] { socket_ready(generation); });
            } catch (...) {
            }
        });
        client.set_close_handler([raw, generation](websocketpp::connection_hdl hdl) {
            std::string reason = "VortiDeck closed the preview connection";
            websocketpp::lib::error_code ec;
            if (auto con = raw->client.get_con_from_hdl(hdl, ec)) {
                if (!con->get_remote_close_reason().empty()) reason = con->get_remote_close_reason();
                const bool rejected = con->get_remote_close_code() == 4401;
                if (!raw->closing) on_ui([generation, reason, rejected] { socket_closed(generation, reason, rejected); });
                return;
            }
            if (!raw->closing) on_ui([generation, reason] { socket_closed(generation, reason, false); });
        });
        client.set_fail_handler([raw, generation](websocketpp::connection_hdl hdl) {
            std::string reason = "Preview connection failed";
            websocketpp::lib::error_code ec;
            if (auto con = raw->client.get_con_from_hdl(hdl, ec)) reason += ": " + con->get_ec().message();
            if (!raw->closing) on_ui([generation, reason] { socket_closed(generation, reason, true); });
        });
        websocketpp::lib::error_code ec;
        auto connection = client.get_connection(url, ec);
        if (ec) {
            log(LOG_WARNING, "invalid preview URL: " + ec.message());
            return false;
        }
        session->hdl = connection->get_handle();
        client.connect(connection);
        session->thread = std::thread([raw] {
            try {
                raw->client.run();
            } catch (const std::exception& error) {
                log(LOG_WARNING, std::string("preview socket loop ended: ") + error.what());
            } catch (...) {
            }
            raw->finished = true;
        });
    } catch (const std::exception& error) {
        log(LOG_WARNING, std::string("could not open the preview socket: ") + error.what());
        return false;
    }
    {
        std::lock_guard lock(g_session_mutex);
        g_session = session;
    }
    g_state.session = session;
    return true;
}

// ---- control messages (UI thread) ----
bool valid_path(const std::string& path) {
    if (path.empty() || path[0] != '/' || path.size() > 128) return false;
    for (const char c : path)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '-' || c == '_')) return false;
    return true;
}

void start_feed(nlohmann::json message, control_endpoint endpoint) {
    stop_all("stopped", "restarted", false);
    const std::string ticket = message.value("ticket", "");
    const std::string path = message.value("path", "/ws/obs-preview");
    g_state.request_id = message.value("request_id", "");
    g_state.requested_fps = message.value("fps", 30) >= 60 ? 60 : 30;
    g_state.encoder.reset();
    g_state.encoder_name.clear();
    g_state.fallback_reason.clear();
    g_state.failed_encoders.clear();
    g_state.overlay_hidden = false;
    g_state.overlay_skips_seen = g_overlay_skips.load();
    if (ticket.empty() || ticket.size() > 256 || !valid_path(path)) {
        g_state.current = phase::connecting; // so the failure is reported
        stop_all("failed", "Malformed preview request", true);
        return;
    }
    if (endpoint.secure || endpoint.host.empty() || !endpoint.port) {
        g_state.current = phase::connecting;
        stop_all("failed", "The control connection has no plain websocket endpoint", true);
        return;
    }
    g_state.generation++;
    g_state.ticket = ticket;
    g_state.path = path;
    g_state.endpoint = endpoint;
    g_state.keepalive.touch(clock::now());
    g_state.current = phase::connecting;
    const auto url = proto::preview_url(false, endpoint.host, endpoint.port, path);
    if (!open_socket(url)) {
        stop_all("failed", "Could not open the preview connection", true);
        return;
    }
    send_status("starting");
}

} // namespace

void initialize(control_sender sender) {
    g_send = std::move(sender);
    if (g_initialized) return;
    g_initialized = true;

    obs_source_info root{};
    root.id = k_root_source_id;
    root.type = OBS_SOURCE_TYPE_INPUT;
    root.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
    root.get_name = root_get_name;
    root.create = root_create;
    root.destroy = root_destroy;
    root.get_width = root_get_width;
    root.get_height = root_get_height;
    root.video_render = root_render;
    obs_register_source(&root);

    obs_output_info output{};
    output.id = k_output_id;
    output.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
    output.get_name = output_get_name;
    output.create = output_create;
    output.destroy = output_destroy;
    output.start = output_start;
    output.stop = output_stop;
    output.encoded_packet = output_packet;
    obs_register_output(&output);
}

nlohmann::json capability() {
    obs_video_info ovi{};
    const double fps = obs_get_video_info(&ovi) && ovi.fps_den ? static_cast<double>(ovi.fps_num) / ovi.fps_den : 30.0;
    return {{"previewFeed",
             {{"version", proto::k_capability_version},
              {"codecs", {"h264"}},
              {"transport", "websocket"},
              {"header_size", proto::k_header_size},
              {"max_fps", fps}}}};
}

bool should_skip_overlay_render() {
    if (!t_preview_pass) return false;
    g_overlay_skips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool handle_control_message(const nlohmann::json& message, const control_endpoint& endpoint) {
    const std::string type = message.value("type", "");
    if (type == "PREVIEW_FEED_START") {
        on_ui([message, endpoint] { start_feed(message, endpoint); });
        return true;
    }
    if (type == "PREVIEW_FEED_KEEPALIVE") {
        on_ui([] {
            if (g_state.current != phase::idle) g_state.keepalive.touch(clock::now());
        });
        return true;
    }
    if (type == "PREVIEW_FEED_STOP") {
        const std::string request_id = message.value("request_id", "");
        on_ui([request_id] {
            if (!request_id.empty()) g_state.request_id = request_id;
            stop_all("stopped", "requested", true);
        });
        return true;
    }
    return false;
}

void on_control_disconnected() {
    on_ui([] { stop_all("stopped", "control connection closed", false); });
}

void tick() {
    if (g_shutting_down || g_state.current == phase::idle) return;
    const auto now = clock::now();
    if (g_state.keepalive.expired(now)) {
        stop_all("stopped", "VortiDeck stopped sending keepalives", true);
        return;
    }
    if (g_state.current != phase::running) return;

    obs_video_info ovi{};
    if (obs_get_video_info(&ovi) &&
        (ovi.base_width != g_state.width || ovi.base_height != g_state.height || ovi.fps_num != g_state.obs_fps_num ||
         ovi.fps_den != g_state.obs_fps_den)) {
        log(LOG_INFO, std::format("canvas changed to {}x{}, reconfiguring", ovi.base_width, ovi.base_height));
        teardown_pipeline();
        send_status("reconfiguring");
        start_pipeline();
        return;
    }

    // A hardware encoder that starts but never produces a packet (driver
    // trouble) falls through to the next candidate, at the same resolution.
    if (g_state.watchdog_armed && g_state.session && now - g_state.pipeline_started > k_start_watchdog) {
        g_state.watchdog_armed = false;
        if (g_state.session->packets.load() == g_state.packets_at_start && g_state.encoder) {
            const std::string failed = g_state.encoder->id;
            g_state.failed_encoders.push_back(failed);
            release_cached_encoder();
            g_state.fallback_reason = std::string(g_state.encoder->family) + ": produced no frames";
            log(LOG_WARNING, "encoder " + failed + " produced no frames; trying the next one");
            teardown_pipeline();
            send_status("reconfiguring", "Encoder produced no frames");
            start_pipeline();
            return;
        }
    }

    const uint64_t skips = g_overlay_skips.load(std::memory_order_relaxed);
    const bool hidden = skips != g_state.overlay_skips_seen;
    g_state.overlay_skips_seen = skips;
    if (hidden != g_state.overlay_hidden) {
        g_state.overlay_hidden = hidden;
        send_status("running");
    }
}

void suspend_for_video_reset() {
    if (g_state.current != phase::running) return;
    teardown_pipeline();
    g_state.current = phase::suspended;
    send_status("suspended", "OBS canvas is being changed");
}

void resume_after_video_reset() {
    if (g_state.current != phase::suspended) return;
    start_pipeline();
}

void shutdown() {
    stop_all("stopped", "OBS is exiting", false);
    release_pipeline_objects();
    g_shutting_down = true;
    g_send = nullptr;
}

} // namespace vortideck::preview_feed
