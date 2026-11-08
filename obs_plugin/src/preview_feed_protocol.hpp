#pragma once
// Pure protocol and policy pieces of the live preview feed. No libobs or
// websocket dependencies, so cmake/tests/preview-feed-contract.cpp can test
// them directly. See obs_plugin/docs/PREVIEW_FEED_PLAN.md.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vortideck::preview {

inline constexpr uint32_t k_magic = 0x46504456; // "VDPF" little-endian
inline constexpr uint8_t k_version = 1;
inline constexpr uint16_t k_header_size = 48;
inline constexpr int k_protocol_version = 2;      // companion control protocol
inline constexpr int k_capability_version = 1;    // previewFeed capability
inline constexpr uint8_t k_flag_keyframe = 1u << 0;
inline constexpr uint8_t k_flag_config = 1u << 1;
inline constexpr uint8_t k_flag_discontinuity = 1u << 2;

struct frame_header {
    uint8_t flags = 0;
    uint32_t sequence = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    int64_t pts_us = 0;
    int64_t render_time_us = 0;
    int64_t send_time_us = 0;
    int64_t relay_epoch_us = 0;
};

namespace detail {
template <typename T> inline void put(uint8_t* out, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) out[i] = static_cast<uint8_t>(static_cast<uint64_t>(value) >> (8 * i));
}
template <typename T> inline T get(const uint8_t* in) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) value |= static_cast<uint64_t>(in[i]) << (8 * i);
    return static_cast<T>(value);
}
} // namespace detail

inline void write_header(uint8_t* out, const frame_header& h) {
    detail::put<uint32_t>(out + 0, k_magic);
    out[4] = k_version;
    out[5] = h.flags;
    detail::put<uint16_t>(out + 6, k_header_size);
    detail::put<uint32_t>(out + 8, h.sequence);
    detail::put<uint16_t>(out + 12, h.width);
    detail::put<uint16_t>(out + 14, h.height);
    detail::put<int64_t>(out + 16, h.pts_us);
    detail::put<int64_t>(out + 24, h.render_time_us);
    detail::put<int64_t>(out + 32, h.send_time_us);
    detail::put<int64_t>(out + 40, h.relay_epoch_us);
}

inline std::optional<frame_header> read_header(std::span<const uint8_t> bytes) {
    if (bytes.size() < k_header_size) return std::nullopt;
    const uint8_t* in = bytes.data();
    if (detail::get<uint32_t>(in) != k_magic || in[4] != k_version) return std::nullopt;
    if (detail::get<uint16_t>(in + 6) != k_header_size) return std::nullopt;
    frame_header h;
    h.flags = in[5];
    h.sequence = detail::get<uint32_t>(in + 8);
    h.width = detail::get<uint16_t>(in + 12);
    h.height = detail::get<uint16_t>(in + 14);
    h.pts_us = detail::get<int64_t>(in + 16);
    h.render_time_us = detail::get<int64_t>(in + 24);
    h.send_time_us = detail::get<int64_t>(in + 32);
    h.relay_epoch_us = detail::get<int64_t>(in + 40);
    return h;
}

// 0.06 bits per pixel per frame, clamped to a range every LAN handles.
inline uint32_t bitrate_kbps(uint32_t width, uint32_t height, uint32_t fps) {
    const double kbps = std::round(static_cast<double>(width) * height * fps * 0.06 / 1000.0);
    return static_cast<uint32_t>(std::clamp(kbps, 2500.0, 20000.0));
}

struct fps_plan {
    uint32_t divisor = 1;
    double effective = 0;
    bool limited_by_obs = false;
};

// The preview view ticks with OBS's own frame rate; a lower preview rate comes
// from the encoder frame-rate divisor. A rate above OBS's is not possible.
inline fps_plan plan_fps(uint32_t obs_fps_num, uint32_t obs_fps_den, uint32_t requested) {
    fps_plan plan;
    const double obs_fps = obs_fps_den ? static_cast<double>(obs_fps_num) / obs_fps_den : 30.0;
    requested = requested >= 60 ? 60 : 30;
    plan.divisor = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(obs_fps / requested)));
    plan.effective = obs_fps / plan.divisor;
    plan.limited_by_obs = plan.effective + 0.5 < requested;
    return plan;
}

struct encoder_candidate {
    const char* id;
    bool hardware;
    const char* family; // nvenc | amf | qsv | x264
};

// Preference order. Texture (zero-copy) hardware encoders first, x264 last.
inline constexpr encoder_candidate k_encoder_order[] = {
    {"obs_nvenc_h264_tex", true, "nvenc"},
    {"jim_nvenc", true, "nvenc"},
    {"h264_texture_amf", true, "amf"},
    {"obs_qsv11_v2", true, "qsv"},
    {"obs_qsv11", true, "qsv"},
    {"obs_x264", false, "x264"},
};

// Candidates that this OBS actually offers, in preference order.
inline std::vector<encoder_candidate> available_encoders(const std::function<bool(const char*)>& is_h264_available) {
    std::vector<encoder_candidate> result;
    for (const auto& c : k_encoder_order)
        if (is_h264_available(c.id)) result.push_back(c);
    return result;
}

struct encoder_choice {
    std::optional<encoder_candidate> chosen;
    std::string fallback_reason; // set when a hardware encoder was skipped
    std::string failure;         // set when nothing started
};

// Try candidates in order; `start` returns an empty string on success or the
// failure reason. The resolution is never changed between attempts.
inline encoder_choice choose_encoder(const std::vector<encoder_candidate>& candidates,
                                     const std::function<std::string(const encoder_candidate&)>& start) {
    encoder_choice result;
    std::string hardware_errors;
    for (const auto& c : candidates) {
        const std::string error = start(c);
        if (error.empty()) {
            result.chosen = c;
            if (!c.hardware) {
                result.fallback_reason = hardware_errors.empty() ? std::string("No hardware H.264 encoder is available")
                                                                 : hardware_errors;
            }
            return result;
        }
        if (c.hardware) {
            if (!hardware_errors.empty()) hardware_errors += "; ";
            hardware_errors += std::string(c.family) + ": " + error;
        } else {
            if (!result.failure.empty()) result.failure += "; ";
            result.failure += std::string(c.family) + ": " + error;
        }
    }
    if (result.failure.empty()) result.failure = hardware_errors.empty() ? "No H.264 encoder is available" : hardware_errors;
    else if (!hardware_errors.empty()) result.failure = hardware_errors + "; " + result.failure;
    return result;
}

struct queued_frame {
    bool keyframe = false;
    std::vector<uint8_t> bytes; // header + payload
};

// Bounded hand-off from the OBS encoder thread to the preview sender. Never
// blocks the producer. On overflow (or when the sender reports back-pressure)
// everything queued is dropped and delta frames are refused until the next
// keyframe, which is then flagged as a discontinuity.
class frame_queue {
public:
    explicit frame_queue(size_t capacity = 8) : m_capacity(capacity) {}

    // Returns false when the frame was dropped.
    bool push(queued_frame frame) {
        if (m_frames.size() >= m_capacity) drop_to_next_keyframe();
        if (m_waiting_for_keyframe && !frame.keyframe) { ++m_dropped; return false; }
        if (m_waiting_for_keyframe) {
            m_waiting_for_keyframe = false;
            if (frame.bytes.size() >= k_header_size) frame.bytes[5] |= k_flag_discontinuity;
        }
        m_frames.push_back(std::move(frame));
        return true;
    }

    std::optional<queued_frame> pop() {
        if (m_frames.empty()) return std::nullopt;
        auto f = std::move(m_frames.front());
        m_frames.pop_front();
        return f;
    }

    // The network is behind: discard the backlog and resync on a keyframe.
    void drop_to_next_keyframe() {
        m_dropped += m_frames.size();
        m_frames.clear();
        m_waiting_for_keyframe = true;
    }

    void clear() { m_frames.clear(); m_waiting_for_keyframe = true; }
    size_t size() const { return m_frames.size(); }
    uint64_t dropped() const { return m_dropped; }
    bool waiting_for_keyframe() const { return m_waiting_for_keyframe; }

private:
    size_t m_capacity;
    std::deque<queued_frame> m_frames;
    bool m_waiting_for_keyframe = true; // a stream always starts on a keyframe
    uint64_t m_dropped = 0;
};

// A crashed or disconnected VortiDeck must not leave OBS's video settings
// locked: the feed stops when keepalives stop arriving.
class keepalive {
public:
    using clock = std::chrono::steady_clock;
    explicit keepalive(std::chrono::milliseconds timeout = std::chrono::seconds(15)) : m_timeout(timeout) {}
    void touch(clock::time_point now) { m_last = now; }
    bool expired(clock::time_point now) const { return now - m_last > m_timeout; }

private:
    std::chrono::milliseconds m_timeout;
    clock::time_point m_last{};
};

// Scope guard for the preview render pass. Nested passes restore the outer
// state, so a preview root rendered inside another preview root is harmless.
class render_pass_scope {
public:
    explicit render_pass_scope(bool& flag) : m_flag(flag), m_previous(flag) { m_flag = true; }
    ~render_pass_scope() { m_flag = m_previous; }
    render_pass_scope(const render_pass_scope&) = delete;
    render_pass_scope& operator=(const render_pass_scope&) = delete;

private:
    bool& m_flag;
    bool m_previous;
};

// ws://host:port/ws + "/ws/obs-preview" -> ws://host:port/ws/obs-preview
inline std::string preview_url(bool secure, const std::string& host, uint16_t port, const std::string& path) {
    std::string p = path.empty() || path[0] != '/' ? "/" + path : path;
    const bool ipv6 = host.find(':') != std::string::npos && (host.empty() || host.front() != '[');
    return std::string(secure ? "wss://" : "ws://") + (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port) + p;
}

} // namespace vortideck::preview
