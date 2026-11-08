# Thread ownership and the OBS companion crashes (2026-11-08)

## What the crash evidence shows

| When | What Windows / OBS recorded | Cause |
|---|---|---|
| 2026-08-28 … 2026-10-11, 10 times | `Application Error`, `ucrtbase.dll`, `0xc0000409` at `abort+0x4e` (the `__fastfail` in `abort()`). No OBS crash report, because `abort()` bypasses OBS's crash handler. | `std::terminate` from the plugin's thread teardown (paths A and B below). |
| 2026-11-08 00:10 | `Application Hang`; the OBS log stops at 00:10:28 while plugin threads keep logging. | Path C below. The log shows `User selected service from dialog` → `Disconnect: Thread join timeout` and then nothing from the UI thread. |
| `Crash 2026-06-21 01-46-48.txt` | Faulting thread: `win-dshow` → `obs.dll!video_frame_init` (allocation failure). | Not plugin code. The plugin thread in that report is blocked in `~jthread` → `join` from `disconnect()` ← `connect()` ← the startup thread, while a detached `disconnect` helper joins the same thread. That is path A in progress: harmless until OBS exits, then `abort()`. |
| `Crash 2026-08-28 02-55-30.txt` | Faulting thread: `CrBrowserMain`, CEF `CHECK` in `CreateBrowserSync` (obs-browser dock). | Not plugin code. The plugin threads show path A at an earlier step: `disconnect()` in its 3 s wait, the helper in `join`, and `_run_forever` alive in its reconnect sleep. |
| `Crash 2026-05-01 18-35-06.txt` | Faulting thread: `CrBrowserMain` in obs-browser's `BrowserManagerThread`. | Not plugin code. The asio timer thread in the report is idle. |
| 2026-11-08 18:54 (dump `obs64.exe.95992.dmp`) | `0xe` from `obs!brealloc` in `obs_source_output_audio` (WASAPI). | Not plugin code: OBS ran out of memory buffering "Desktop Audio", which the log had reported lagging by minutes since 15:29. |

In the three OBS crash reports the plugin frames are on threads that did not fault. They still show the plugin's broken teardown state, which is why the reports looked like plugin crashes.

## The defect (v1.0.11 and earlier)

`_run_forever()` ran `while (!m_shutting_down)` and ignored its `stop_token`. After `m_websocket.stop()`, `run()` returned, the loop reset asio and reconnected. The connection thread therefore never ended before OBS did. `disconnect()` still tried to stop and join it:

1. It stopped asio and requested a stop that nothing checked.
2. It started a `std::jthread` helper to `join()` the connection thread and waited 3 s.
3. On timeout it detached the helper, which kept joining and still held a reference to a stack variable of the finished `disconnect()`.
4. It called `m_websocket.reset()` while the connection thread was using asio.
5. It ran `m_websocket_thread.reset()`, whose `~jthread` joins again.

Who called `disconnect()`, and what happened:

- **A, startup (double join).** OBS starts while VortiDeck is down or slow, so the detached startup thread's `connect()` times out after 3 s and calls `disconnect()`. The startup thread and the helper now both block in `join()` on the same thread. At OBS exit `_run_forever` finally returns and both wake. MSVC's `_Thrd_join` closes the handle in one joiner, and the other fails (`no such process`), which throws inside a `noexcept` destructor or a thread body: `std::terminate` → `abort()` → `0xc0000409`.
- **B, reconnect (self join).** `websocket_open_handler()` calls `disconnect()` when registration cannot be sent, which happens when VortiDeck drops right after accepting. That runs on the connection thread itself, so `~jthread` joins its own thread: `resource_deadlock_would_occur` → `std::terminate` → `abort()`.
- **C, service dialog (hang).** "Accept" in the service dialog runs `reconnect()` → `disconnect()` on the Qt UI thread, which then waits in `~jthread::join` forever: Application Hang.

The status loop thread had the same kind of race in a smaller form: `start_loop()`/`stop_loop()` ran on the UI thread, the connection thread and the startup thread without a lock on the `std::jthread` object.

## The rules now

`obs_plugin/src/connection_lifecycle.hpp`:

- `owned_thread` is the only owner of a thread. `stop()` hands the thread to exactly one caller, detaches instead of joining when called on the thread itself, and waits for a completion signal (bounded) before `join()`. Its destructor never blocks or terminates; it may run under the loader lock. No exception escapes a thread body.
- `run_control_connection()` is the connection loop: open, run, reset, sleep on the stop token, repeat. The reset happens before the stop check, so an asio stop at exit is never undone.

| Thread | Owner | Started by | Stopped by |
|---|---|---|---|
| connection (`_run_forever`) | `m_websocket_thread` | `connect()` (post_load; idempotent) | `shutdown_control_connection()`: OBS EXIT event (and unload as a fallback). Sets the flag, requests stop, stops asio, then joins. |
| status loop | `m_loop_thread` | `start_loop()`: post_load, activation | `stop_loop()`: `disconnect()`, fail handler, EXIT |
| mDNS discovery | `m_continuous_discovery_thread` | `start_continuous_discovery()` | `stop_continuous_discovery()`: EXIT |
| startup thread | removed | n/a | n/a |

`disconnect()` now only clears state, stops the status loop and closes the current websocket connection. It never stops asio and never joins the connection thread, so it is safe from any thread, including the connection thread itself. `reconnect()` is `disconnect()` + `connect()` and returns immediately; the connection thread reconnects with the newly selected URL. The 1 s startup delay and the 5 s wait for discovery moved into the connection thread, where its stop token can interrupt them.

Remaining bound: at EXIT, if a message handler is blocked in an OBS frontend call that waits for the UI thread, the join gives up after 3 s and releases the thread, which is logged as `Websocket thread released (did not finish in time)`.

## Verification

`cmake/tests/connection-lifecycle-stress.cpp` (ctest `connection-lifecycle-stress`) runs a local websocket server that keeps going up and down, closing cleanly or vanishing. A client with this lifecycle is driven by "UI" disconnects and reconnects, a second thread that starts and stops everything, and disconnects from inside the open handler. It must end with every thread joined and no call blocking for more than 2.5 s.

The pre-fix code is kept in the same file:

```text
connection-lifecycle-stress --legacy double-join   -> TERMINATE (no such process)            exit 3
connection-lifecycle-stress --legacy self-join     -> TERMINATE (resource deadlock would occur) exit 3
connection-lifecycle-stress --legacy hang          -> HANG (UI-thread disconnect never returns) exit 4
connection-lifecycle-stress --seconds 20            -> PASS (about 260 loop joins, connection thread joined in 0 ms)
```
