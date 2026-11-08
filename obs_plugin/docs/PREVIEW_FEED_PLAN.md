# Live OBS preview feed: plan (draft for approval, 2026-11-08)

Goal: the VortiDeck overlay editor shows the streamer's live OBS program scene,
minus VortiDeck's own overlay, at 30 fps or more, 1:1 behind the editor canvas.
The OBS companion plugin streams it. Nothing in the real scenes, the stream or
the recording is touched.

Measured context on the development PC: OBS 32.2.2 (the plugin compiles
against the 30.1.2 SDK in `.deps`), base canvas 2560x1440 at 30/1 fps, an RTX
5070 Ti, and `obs_nvenc_h264_tex`, AMF, QSV and x264 all installed.

All libobs calls below were checked against `.deps/obs-studio-30.1.2/libobs`
(`obs.h`, `obs-output.h`, `obs-encoder.h`, `obs-view.c`, `obs-video.c`,
`obs-output.c`). Every one of them also exists in OBS 31 and 32.

## 1. Picture: the program scene without the overlay, with nothing duplicated

**Rejected: `obs_scene_duplicate(..., OBS_SCENE_DUP_PRIVATE_REFS)`.** It only
copies the top-level scene. Nested scenes and groups are still shared with the
real ones, so hiding an overlay that sits inside a nested scene would hide it on
stream too. The copy would also need re-syncing on every item add, remove,
reorder or transform, and it would not reproduce transitions.

**Chosen: hide the overlay in our own preview render pass.** `vortideck_overlay`
is our own source type, so its `video_render` decides what it draws.

- `preview_feed.cpp` registers a private source type, `vortideck_preview_root`
  (`OBS_SOURCE_VIDEO`), and creates one instance with
  `obs_source_create_private`. It never appears in the OBS UI.
- Its `video_render` does three things:
  1. sets a `thread_local bool t_preview_pass = true`;
  2. calls `obs_source_video_render(obs_get_output_source(0))`, which is the
     program transition, so scene switches, transitions and studio mode
     (program, not preview) all follow automatically;
  3. clears the flag and releases the reference.

  `get_width`/`get_height` return the base canvas size.
- `overlay_source_render` (`sources/overlay_source.cpp:450`) returns without
  drawing while `preview_feed::in_preview_pass()` is true. The main mix renders
  with the flag false, so the stream and recording are unchanged. This covers
  every depth of nesting, groups and filters, because rendering is synchronous
  on the graphics thread.
- The root source is set on a private view:
  - `obs_view_create()`;
  - `obs_view_set_source(view, 0, root)`;
  - `obs_view_add2(view, &ovi)`, which returns `video_t*` (it exists since
    OBS 30.0, and Aitum Vertical uses it for its second canvas).

  `obs-video.c` renders each view mix with `obs_view_render`. The texture of
  another mix is reused only when the views are identical
  (`can_reuse_mix_texture`), which ours never is.
- `ovi` is a copy of `obs_get_video_info()` with:
  - base and output size set to the base canvas size (1:1, no scaling, the
    already-synced size);
  - `output_format = VIDEO_FORMAT_NV12`, forced only if the main format is
    I444, P010 or another format H.264 hardware encoders can't take;
  - `colorspace = VIDEO_CS_709` if the main canvas is HDR (2100 PQ/HLG);
  - `gpu_conversion = true`;
  - `scale_type` as in the main mix.

  The view uses the main fps (`obs-video.c` ticks every mix at the main
  cadence). Lower preview fps comes from `obs_encoder_set_frame_rate_divisor`.
- Teardown order:
  1. stop the output;
  2. `obs_view_remove(view)`; libobs frees the mix on the next
     `output_frames()` pass;
  3. `obs_view_set_source(view, 0, nullptr)`;
  4. `obs_view_destroy(view)`;
  5. release the root source.

**Known limitation:** an overlay that a streamer added as a plain Browser Source
(not the "VortiDeck Overlay" source) can't be skipped this way. When the root
renders and no `vortideck_overlay` was drawn in the main pass, the plugin
reports `overlay_hidden=false` and the editor shows a short hint.

## 2. Encoding

- **Our own output type**, `vortideck_preview_output`, registered with
  `obs_register_output`:
  - flags `OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED` (video only, no service);
  - `start`: `obs_output_can_begin_data_capture`, then
    `obs_output_initialize_encoders`, then `obs_output_begin_data_capture`;
  - `stop`: `obs_output_end_data_capture`;
  - `encoded_packet`: copies the packet into the preview queue and returns at
    once.

  `video_valid()` in `obs-output.c` only requires a video encoder for encoded
  outputs, so no audio and no `obs_output_set_media` are needed.
- **Encoder:** `obs_video_encoder_create(id, "vortideck_preview", settings, nullptr)`,
  then `obs_encoder_set_video(enc, view_video)`, then
  `obs_output_set_video_encoder(out, enc)`.
- **Encoder selection.** Candidates, in order, keeping only IDs that
  `obs_enum_encoder_types` lists with `obs_get_encoder_codec(id) == "h264"`:
  1. `obs_nvenc_h264_tex` (OBS 31+) and `jim_nvenc` (OBS 30);
  2. `h264_texture_amf`;
  3. `obs_qsv11_v2` and `obs_qsv11`;
  4. `obs_x264`.

  If `obs_output_start` fails (NVENC session limit, driver error, no hardware),
  the plugin releases that encoder and tries the next. When it ends on x264,
  `PREVIEW_FEED_STATUS.encoder.fallback_reason` carries
  `obs_output_get_last_error`. **The resolution is never changed.** If even
  x264 can't start, the state is `failed` with the reason.
- **Settings.** A key is set only if `obs_encoder_defaults(id)` has it
  (`obs_data_has_default_value`), so key names that differ across OBS versions
  are skipped rather than guessed:
  - `rate_control=CBR`, `bitrate=<kbps>`, `keyint_sec=1`, B-frames 0
    (`bf` / `bframes`);
  - NVENC: `preset=p1`, `preset2=p1`, `tune=ull`, `multipass=disabled`,
    `lookahead=false`, `profile=high`;
  - AMF: `preset=speed`, `profile=high`;
  - QSV: `target_usage=TU7`, `latency=ultra-low`;
  - x264: `preset=ultrafast`, `tune=zerolatency`, `profile=high`.
- **Bitrate:** `clamp(round(w*h*fps*0.06/1000), 2500, 20000)` kbps, which gives
  6636 kbps at 2560x1440 and 30 fps, and 13271 kbps at 60 fps.
- **FPS:**
  - `requested` is 30 (default) or 60;
  - `divisor = max(1, round(obs_fps / requested))`;
  - `effective = obs_fps / divisor`.

  For example, OBS at 60 with 30 requested gives divisor 2. OBS at 30 with 60
  requested gives 30 fps, reported as `fps_limited_by_obs`.
- **SPS/PPS:** `obs_encoder_get_extra_data` (Annex B) is prepended to every
  keyframe, so a decoder can join or recover at any keyframe.

## 3. Transport

### Control messages (existing authenticated control socket, JSON)

- **Registration** gains `"protocolVersion": 2` and
  `"capabilities": {"previewFeed": {"version": 1, "codecs": ["h264"], "max_fps": <obs fps>}}`.
  VortiDeck treats a missing field as version 1 with no preview.
- **Host to plugin:**
  - `PREVIEW_FEED_START {request_id, ticket, fps, path:"/ws/obs-preview"}`.
    The ticket is 32 random bytes, base64url.
  - `PREVIEW_FEED_KEEPALIVE {}` every 5 s while the editor shows the preview.
  - `PREVIEW_FEED_STOP {request_id}`.
- **Plugin to host:**
  `PREVIEW_FEED_STATUS {state: starting|running|reconfiguring|suspended|stopped|failed, reason, encoder:{id, name, hardware, fallback_reason}, width, height, fps, requested_fps, bitrate_kbps, keyint_sec, overlay_hidden}`,
  sent on every change.

### Preview socket (separate connection, own thread)

- The plugin opens a second websocketpp client, with its own `asio::io_context`
  and worker thread, to the same `host:port` as the control URL plus `path`.
- First frame: `{"type":"PREVIEW_HELLO","ticket":...}`. The ticket is
  single-use and expires after 30 s. VortiDeck compares it in constant time and
  binds it to the companion connection that requested it. Only an authenticated
  control session can mint one, so the HMAC pairing extends to the preview
  socket without a second handshake.
- VortiDeck replies `PREVIEW_READY`, or closes with 4401.
- After that, binary frames only. Each frame has a 48-byte little-endian header
  followed by one Annex B H.264 access unit:

| off | type | field |
|----:|------|-------|
| 0 | u32 | magic `VDPF` |
| 4 | u8 | version = 1 |
| 5 | u8 | flags: bit0 keyframe, bit1 has SPS/PPS, bit2 discontinuity |
| 6 | u16 | header length = 48 |
| 8 | u32 | sequence |
| 12 | u16 | width |
| 14 | u16 | height |
| 16 | i64 | pts in microseconds |
| 24 | i64 | render time in microseconds (`encoder_packet.sys_dts_usec`, `os_gettime_ns` clock) |
| 32 | i64 | send time in microseconds (`os_gettime_ns()/1000`) |
| 40 | i64 | relay arrival time in Unix-epoch microseconds (0 from the plugin; VortiDeck fills it in) |

### Isolation and back-pressure

- The OBS encoder thread only copies data into a bounded queue (8 frames, or
  about 1 s) and returns.
- The preview worker sends. If the queue overflows, or websocketpp's buffered
  amount goes over 2 MB, it drops everything queued, skips to the next keyframe
  (at most 1 s away) and sets the discontinuity flag.
- A stalled or failed preview socket never touches `m_websocket`, `m_lock` or
  the control thread. Failures are logged and reported in `PREVIEW_FEED_STATUS`
  over the control socket.

### Lifecycle

- Fully idle until START: no view, encoder, output, socket or thread.
- Creating and destroying OBS objects runs on the OBS UI thread
  (`QMetaObject::invokeMethod`, as canvas Apply already does), never on a
  websocket thread.
- The preview stops when any of these happens:
  - STOP;
  - no keepalive for 15 s (a crashed VortiDeck can't leave OBS's video settings
    locked);
  - the control connection closes;
  - the preview socket closes;
  - `OBS_FRONTEND_EVENT_EXIT`.
- **Canvas changes:** the existing 1 s canvas observer calls
  `preview_feed::on_canvas_observed(w, h)`. A different base size restarts the
  view and encoder at the new size and sends `reconfiguring` then `running`.
- **Canvas Apply:** `obs_video_active()` counts every mix (`obs.c:3248`), so
  OBS refuses a video reset while the preview encodes. `handle_canvas_apply_request`
  calls `preview_feed::suspend()` before `obs_frontend_reset_video()` and
  `resume()` afterwards, on success and on failure.
- **Trade-off:** while the preview runs, OBS's own Settings > Video page is
  locked, just as it is while recording. That is why the preview is off by
  default, pauses when the editor is hidden and uses the keepalive timeout.

## 4. VortiDeck

- **`libs/vortideck-core/src/services/obs/preview_feed.rs`** (pure, unit
  tested):
  - capability parsing;
  - the ticket store (single use, TTL, constant-time compare);
  - header parsing and validation (magic, version, sizes, the frame matches the
    announced canvas);
  - keyframe gating after a gap;
  - fps, kbps and latency statistics.
- **`src-tauri` (thin wiring, one batch of edits, which restarts the dev app
  once):**
  - a `warp::path!("ws" / "obs-preview")` route placed before `ws_route`, with
    Ping/Pong handled so the stream stays open (the shared adapter currently
    ends the stream on Ping);
  - companion capability stored on registration and cleared in
    `disconnect_obs_companion`;
  - commands `obs_preview_start(channel: tauri::ipc::Channel<InvokeResponseBody>, fps)`
    and `obs_preview_stop()`; frames go out as `InvokeResponseBody::Raw` (an
    ArrayBuffer in JS);
  - the `obs-preview-status` event;
  - the keepalive timer;
  - one subscriber at a time, where a new subscription replaces the old one.
- **Editor:**
  - `composables/overlay/useObsLivePreview.ts` (with a spec):
    - opens the Channel;
    - `VideoDecoder` with `hardwareAcceleration: 'prefer-hardware'`,
      `optimizeForLatency: true`, and the codec string taken from the SPS
      (`avc1.PPCCLL`);
    - draws to a `<canvas>` the size of `canvasWidth × canvasHeight` behind the
      elements and the grid;
    - drops delta frames while `decodeQueueSize > 2` until the next keyframe;
    - closes every `VideoFrame`.
  - It pauses (STOP) on `visibilitychange` hidden, on route leave or unmount,
    or when the toggle is off, and resumes on return.
  - A "Live OBS preview" switch in the `OverlayCanvas` toolbar, next to
    "Live data", off by default. It is UI state only: never part of the overlay
    document or the export (a test asserts this).
  - A status chip, for example "NVENC · 2560×1440 · 30 fps", or
    "Software encoding (x264): NVENC unavailable: <reason>", "Update the OBS
    companion to see your scene", or the plain Browser Source hint.
  - If the OBS canvas and the editor canvas differ (they shouldn't once
    synced), the preview is fitted with letterboxing and the existing mismatch
    hint is shown. The preview is never resized silently.

## 5. Fallback without the capability

VortiDeck has no obs-websocket client today. The fallback needs:

- an obs-websocket v5 client in core (SHA-256 challenge authentication, with the
  password entered by the user and kept in the OS credential store);
- `GetSceneItemList` and `GetSceneItemTransform` for the program scene;
- `GetSourceScreenshot` (JPEG) for each visible top-level item whose input kind
  isn't `vortideck_overlay`, at 2–5 fps, composed in the editor with each item's
  position, scale, rotation, crop and bounds.

Limits: nested scenes are captured whole, so an overlay inside one stays
visible, and filters and blend modes are only approximate.

## 6. Two-PC setups

The preview socket goes to the same discovered `host:port` as the control
socket, so it works across a LAN wherever the companion already connects.

- Non-loopback companions are always HMAC-paired, which the ticket inherits.
- Bandwidth is about 6.6 Mbit/s at 1440p30.
- Verification:
  1. connect by the LAN address (192.168.1.203) instead of loopback, which
     forces the remote, paired path;
  2. if a second PC is available, a real two-PC run.

## 7. Tests and measurement

**Plugin** (`ctest`): a new `preview-feed-contract` executable, pure logic and
no OBS process, covering:

- header round trip;
- the bitrate table;
- encoder order and fallback with simulated availability and start failures;
- fps divisor and limit;
- queue overflow, which drops to the next keyframe with the discontinuity flag;
- the keepalive timeout;
- the overlay skip flag scope (a nested pass sets and restores it).

**VortiDeck:**

- core unit tests for the ticket store, header validation, keyframe gating and
  capability parsing;
- a src-tauri test of the route rejecting a bad or reused ticket;
- Vitest specs for the composable: toggle off by default, STOP on hide and
  unmount, channel closed, frames closed, drop-to-keyframe;
- the overlay serializer excludes preview state.

**Measurement** (needs your OK to load the test DLL into OBS and to restart the
dev app):

- fps at the relay and at decoder output;
- latency from render to relay, and from relay to decoded frame;
- CPU of `obs64`, VortiDeck and WebView2 (`Get-Counter`);
- GPU encode, decode and 3D (`nvidia-smi dmon` and GPU Engine counters);
- at 2560x1440 at 30, with idle as the baseline, NVENC, then forced x264.

## Files

- **Plugin:**
  - `obs_plugin/src/preview_feed.{hpp,cpp}` (new);
  - `preview_feed_protocol.hpp` (new; pure header and policy, shared with the
    test);
  - hooks in `obs_plugin.cpp`: registration capability, START, STOP and
    KEEPALIVE dispatch, disconnect, EXIT, canvas observer, Apply
    suspend/resume;
  - `sources/overlay_source.cpp` (the render skip);
  - `CMakeLists.txt` (sources and the test);
  - `cmake/tests/preview-feed-contract.cpp`.
- **VortiDeck:**
  - core `services/obs/preview_feed.rs`;
  - src-tauri `local_server.rs`, `warp_adapters.rs`, `main.rs` (the companion
    message hook), and a new `commands/obs_preview_commands.rs`;
  - `layers/desktop-streaming/composables/overlay/useObsLivePreview.ts` and its
    spec;
  - `OverlayCanvas.vue` and `pages/streaming/overlays/index.vue`.

## Implemented and measured (2026-11-08)

Setup: OBS 32.2.2, 2560x1440 at 30 fps, RTX 5070 Ti, VortiDeck dev build on the
same PC. The companion connected over the LAN address (`ws://192.168.1.203:9001`),
which is the remote, paired path. Each run was 30 s.

Changes made after the first measurements:

- **Constrained Baseline profile** for every encoder (no frame reordering).
- **Software decode first in the editor**, with hardware as the fallback.
  WebView2's hardware H.264 path holds about 7 frames (234 ms at 1440p30),
  even with Baseline and `optimizeForLatency`. The software decoder outputs
  each frame in about 4 ms.
- **Long-lived objects are reused.** The root source and the view stay for
  the OBS session, and the encoder/output pair is kept while its settings
  match. Only the video mix is added and removed per start/stop. OBS's exit
  leak count went from about 9 per preview start to about 1 (48 after 7
  starts; sessions without the preview end at 39–40).

| | NVENC (default) | x264 (forced via `VORTIDECK_PREVIEW_ENCODER=obs_x264`) |
|---|---|---|
| fps at relay / decoded | 30.0 / 30.0, 0 dropped | 30.0 / 30.0, 0 dropped |
| bitrate | 6.6 Mbit/s | 6.6 Mbit/s |
| OBS render to relay (p50 / p95) | 134 / 135 ms (OBS's NVENC keeps about 4 frames in flight) | 36 / 38 ms |
| relay to editor (Tauri Channel) | 2.4 ms | 2.9 ms |
| editor decode and draw (software) | 4 ms | 2 ms |
| total, render to on screen | about 140 ms | about 41 ms |
| CPU, % of all 24 threads (baseline 0.86 OBS / 0.12 VortiDeck / 3.23 WebView2) | 0.80 / 0.23 / 3.52 | 2.47 / 0.33 / 3.84 |
| GPU (baseline SM 21%, encode 2%, decode 2%) | SM 20%, encode 2.6%, decode 0% | SM 13%, encode 0%, decode 0% |

- **Overlay hidden:** `overlay_hidden: true` was confirmed with the main
  VortiDeck Overlay source in the scene.
- **Frame rate:** a 60 fps request on a 30 fps OBS reports
  `fps_limited_by_obs`.
- **Not verified live:**
  - a canvas Apply while the preview runs (suspend/resume): the user's real
    canvas was not changed;
  - a real second PC.
