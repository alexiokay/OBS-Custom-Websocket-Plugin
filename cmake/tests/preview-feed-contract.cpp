#include "preview_feed_protocol.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace vortideck::preview;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static queued_frame frame(bool key, uint32_t seq) {
    queued_frame f;
    f.keyframe = key;
    f.bytes.resize(k_header_size + 4);
    frame_header h;
    h.flags = key ? k_flag_keyframe : 0;
    h.sequence = seq;
    write_header(f.bytes.data(), h);
    return f;
}

int main() {
    try {
        {
            frame_header h;
            h.flags = k_flag_keyframe | k_flag_config;
            h.sequence = 0xA1B2C3D4u;
            h.width = 2560;
            h.height = 1440;
            h.pts_us = -5;
            h.render_time_us = 123456789012345LL;
            h.send_time_us = 123456789012999LL;
            uint8_t bytes[k_header_size] = {};
            write_header(bytes, h);
            require(bytes[0] == 'V' && bytes[1] == 'D' && bytes[2] == 'P' && bytes[3] == 'F', "magic spells VDPF");
            const auto back = read_header(bytes);
            require(back && back->sequence == h.sequence && back->width == 2560 && back->height == 1440 &&
                        back->pts_us == -5 && back->render_time_us == h.render_time_us &&
                        back->send_time_us == h.send_time_us && back->relay_epoch_us == 0 && back->flags == h.flags,
                    "header round trip");
            require(!read_header(std::span<const uint8_t>(bytes, k_header_size - 1)), "short header rejected");
            bytes[4] = 9;
            require(!read_header(bytes), "unknown version rejected");
        }
        {
            require(bitrate_kbps(2560, 1440, 30) == 6636, "1440p30 bitrate");
            require(bitrate_kbps(2560, 1440, 60) == 13271, "1440p60 bitrate");
            require(bitrate_kbps(1280, 720, 30) == 2500, "small canvas floor");
            require(bitrate_kbps(7680, 4320, 60) == 20000, "8k ceiling");
        }
        {
            auto p = plan_fps(30, 1, 30);
            require(p.divisor == 1 && p.effective == 30 && !p.limited_by_obs, "30 on 30");
            p = plan_fps(60, 1, 30);
            require(p.divisor == 2 && p.effective == 30 && !p.limited_by_obs, "30 on 60 halves");
            p = plan_fps(30, 1, 60);
            require(p.divisor == 1 && p.effective == 30 && p.limited_by_obs, "60 on 30 is limited and reported");
            p = plan_fps(60000, 1001, 60);
            require(p.divisor == 1 && !p.limited_by_obs, "59.94 counts as 60");
            p = plan_fps(120, 1, 60);
            require(p.divisor == 2 && p.effective == 60, "60 on 120");
            p = plan_fps(30, 1, 45);
            require(p.divisor == 1 && p.effective == 30, "odd requests fall back to 30");
        }
        {
            const auto only = available_encoders([](const char* id) {
                return std::string(id) == "obs_x264" || std::string(id) == "obs_nvenc_h264_tex" || std::string(id) == "obs_qsv11_v2";
            });
            require(only.size() == 3 && std::string(only[0].id) == "obs_nvenc_h264_tex" &&
                        std::string(only[1].id) == "obs_qsv11_v2" && std::string(only[2].id) == "obs_x264",
                    "available encoders keep preference order");

            auto ok = choose_encoder(only, [](const encoder_candidate&) { return std::string(); });
            require(ok.chosen && std::string(ok.chosen->id) == "obs_nvenc_h264_tex" && ok.fallback_reason.empty(), "hardware first");

            auto limit = choose_encoder(only, [](const encoder_candidate& c) {
                return c.hardware ? std::string("session limit") : std::string();
            });
            require(limit.chosen && std::string(limit.chosen->id) == "obs_x264", "falls back to x264");
            require(limit.fallback_reason == "nvenc: session limit; qsv: session limit", "fallback reason names every skipped encoder");

            const std::vector<encoder_candidate> sw_only{k_encoder_order[5]};
            auto none = choose_encoder(sw_only, [](const encoder_candidate&) { return std::string(); });
            require(none.chosen && none.fallback_reason == "No hardware H.264 encoder is available", "no hardware is reported");

            auto broken = choose_encoder(only, [](const encoder_candidate&) { return std::string("init failed"); });
            require(!broken.chosen && broken.failure.find("x264: init failed") != std::string::npos &&
                        broken.failure.find("nvenc: init failed") != std::string::npos,
                    "total failure keeps every reason");
            auto empty = choose_encoder({}, [](const encoder_candidate&) { return std::string(); });
            require(!empty.chosen && !empty.failure.empty(), "no encoders at all");
        }
        {
            frame_queue q(3);
            require(!q.push(frame(false, 1)), "stream starts on a keyframe");
            require(q.push(frame(true, 2)), "first keyframe accepted");
            require(q.push(frame(false, 3)) && q.push(frame(false, 4)), "deltas accepted");
            require(!q.push(frame(false, 5)), "overflow drops the delta");
            require(q.size() == 0 && q.waiting_for_keyframe(), "overflow drops the backlog");
            require(!q.push(frame(false, 6)), "deltas refused until keyframe");
            require(q.push(frame(true, 7)), "keyframe resumes");
            const auto resumed = q.pop();
            require(resumed && (resumed->bytes[5] & k_flag_discontinuity) && (resumed->bytes[5] & k_flag_keyframe),
                    "resumed keyframe flags a discontinuity");
            require(q.dropped() == 6, "drop count covers refused and discarded frames");

            q.push(frame(false, 8));
            q.drop_to_next_keyframe();
            require(q.size() == 0 && !q.push(frame(false, 9)), "back-pressure resync waits for a keyframe");

            frame_queue full(2);
            full.push(frame(true, 1));
            full.push(frame(false, 2));
            require(full.push(frame(true, 3)), "keyframe at overflow is kept");
            require(full.size() == 1 && (full.pop()->bytes[5] & k_flag_discontinuity), "and flagged as discontinuity");
        }
        {
            keepalive k(std::chrono::seconds(15));
            const auto t0 = keepalive::clock::time_point{} + std::chrono::hours(1);
            k.touch(t0);
            require(!k.expired(t0 + std::chrono::seconds(15)), "within timeout");
            require(k.expired(t0 + std::chrono::seconds(16)), "expires after timeout");
            k.touch(t0 + std::chrono::seconds(16));
            require(!k.expired(t0 + std::chrono::seconds(20)), "touch renews");
        }
        {
            bool flag = false;
            {
                render_pass_scope outer(flag);
                require(flag, "pass sets flag");
                {
                    render_pass_scope inner(flag);
                    require(flag, "nested pass keeps flag");
                }
                require(flag, "nested pass restores outer state");
            }
            require(!flag, "pass clears flag");
        }
        {
            require(preview_url(false, "192.168.1.203", 9001, "/ws/obs-preview") == "ws://192.168.1.203:9001/ws/obs-preview", "lan url");
            require(preview_url(false, "::1", 9001, "ws/obs-preview") == "ws://[::1]:9001/ws/obs-preview", "ipv6 url");
            require(preview_url(true, "[::1]", 9001, "/x") == "wss://[::1]:9001/x", "bracketed ipv6");
        }
        std::cout << "Preview feed header, bitrate, fps, encoder fallback, queue, keepalive and render pass passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "preview-feed-contract failed: " << error.what() << "\n";
        return 1;
    }
}
