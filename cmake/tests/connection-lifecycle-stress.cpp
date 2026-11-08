// Stress test for the control-connection thread lifecycle
// (obs_plugin/src/connection_lifecycle.hpp, docs/THREAD_OWNERSHIP.md).
//
// A local websocket server plays VortiDeck: it comes up and goes down again
// and again, sometimes closing cleanly and sometimes vanishing. A client
// model drives the same thread lifecycle as obs_plugin.cpp:
//   - connection thread: owned_thread + run_control_connection
//   - status loop thread: owned_thread, started on open, stopped on disconnect
//   - disconnect(): stop the loop, close the current connection, never join
//   - shutdown(): flag, stop request, asio stop, join
// While the server flaps, disconnect()/reconnect()/start_loop()/stop_loop()
// are called from the "UI" thread, from a second helper thread and from the
// connection thread itself (inside the open handler, as
// websocket_open_handler() does when registration fails).
//
// Default mode must finish with every thread joined and no hang.
//
// --legacy <scenario> runs the pre-fix disconnect() (copied from
// obs_plugin.cpp v1.0.11) to reproduce the field failures:
//   double-join  startup connect() times out -> disconnect() hangs joining the
//                connection thread; at OBS exit two threads join it -> abort
//   self-join    disconnect() from the open handler -> ~jthread joins itself
//                -> abort
//   hang         reconnect() from the UI thread while connected -> blocks
//                forever (the 2026-11-08 00:10 "Application Hang")
// A legacy run is expected to end with "TERMINATE" or "HANG"; it is not part
// of ctest.

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#ifndef _WEBSOCKETPP_CPP11_TYPE_TRAITS_
#define _WEBSOCKETPP_CPP11_TYPE_TRAITS_
#endif
#define _WEBSOCKETPP_CPP11_RANDOM_DEVICE_

#pragma warning(push)
#pragma warning(disable : 4267 4244 4996)
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/server.hpp>
#pragma warning(pop)

#include "connection_lifecycle.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std::chrono_literals;
using ws_client = websocketpp::client<websocketpp::config::asio_client>;
using ws_server = websocketpp::server<websocketpp::config::asio>;
using vortideck::lifecycle::owned_thread;
using vortideck::lifecycle::stop_result;

namespace {

void say(const std::string& text)
{
    std::fprintf(stdout, "%s\n", text.c_str());
    std::fflush(stdout);
}

[[noreturn]] void fail(const std::string& text)
{
    say("FAIL: " + text);
    std::_Exit(1);
}

void on_terminate()
{
    std::string reason = "no active exception";
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            reason = ex.what();
        } catch (...) {
            reason = "non-standard exception";
        }
    }
    say("TERMINATE: std::terminate called (" + reason + ") - in OBS this is abort(), 0xc0000409 in ucrtbase");
    std::_Exit(3);
}

unsigned short pick_free_port()
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    return acceptor.local_endpoint().port();
}

// ---------------------------------------------------------------- server ---

// Plays VortiDeck: up for a while, then down (clean close or abrupt stop).
class flapping_server {
public:
    explicit flapping_server(unsigned short port) : m_port(port) {}

    void start(std::chrono::milliseconds up_min, std::chrono::milliseconds up_max)
    {
        m_up_min = up_min;
        m_up_max = up_max;
        m_controller = std::thread([this] { control(); });
    }

    void stop()
    {
        m_stop = true;
        if (m_controller.joinable()) m_controller.join();
    }

    void bring_up()
    {
        auto server = std::make_unique<ws_server>();
        server->clear_access_channels(websocketpp::log::alevel::all);
        server->clear_error_channels(websocketpp::log::elevel::all);
        server->init_asio();
        server->set_reuse_addr(true);
        server->set_open_handler([this, s = server.get()](websocketpp::connection_hdl hdl) {
            {
                std::lock_guard lock(m_mutex);
                m_connections.insert(hdl);
            }
            websocketpp::lib::error_code ec;
            s->send(hdl, "hello", websocketpp::frame::opcode::text, ec);
        });
        server->set_close_handler([this](websocketpp::connection_hdl hdl) {
            std::lock_guard lock(m_mutex);
            m_connections.erase(hdl);
        });
        server->set_message_handler([](websocketpp::connection_hdl, ws_server::message_ptr) {});
        websocketpp::lib::error_code ec;
        server->listen(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), m_port), ec);
        if (ec) return;  // port still in TIME_WAIT etc.; try next cycle
        server->start_accept(ec);
        m_server = std::move(server);
        m_io = std::thread([s = m_server.get()] { s->run(); });
        ++m_up_count;
    }

    void bring_down(bool clean)
    {
        if (!m_server) return;
        websocketpp::lib::error_code ec;
        m_server->stop_listening(ec);
        if (clean) {
            std::vector<websocketpp::connection_hdl> open;
            {
                std::lock_guard lock(m_mutex);
                open.assign(m_connections.begin(), m_connections.end());
            }
            for (auto& hdl : open) m_server->close(hdl, websocketpp::close::status::going_away, "bye", ec);
            std::this_thread::sleep_for(30ms);
        }
        m_server->stop();
        if (m_io.joinable()) m_io.join();
        m_server.reset();  // closes every socket that is left
        std::lock_guard lock(m_mutex);
        m_connections.clear();
    }

    int up_count() const { return m_up_count; }

private:
    void control()
    {
        std::mt19937 rng(1234);
        while (!m_stop) {
            bring_up();
            sleep_random(rng, m_up_min, m_up_max);
            bring_down(rng() % 2 == 0);
            sleep_random(rng, 20ms, 250ms);
        }
        bring_down(false);
    }

    void sleep_random(std::mt19937& rng, std::chrono::milliseconds lo, std::chrono::milliseconds hi)
    {
        const auto span = static_cast<unsigned>((hi - lo).count() + 1);
        const auto end = std::chrono::steady_clock::now() + lo + std::chrono::milliseconds(rng() % span);
        while (!m_stop && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(10ms);
    }

    unsigned short m_port;
    std::chrono::milliseconds m_up_min{100}, m_up_max{400};
    std::atomic<bool> m_stop{false};
    std::thread m_controller;
    std::unique_ptr<ws_server> m_server;
    std::thread m_io;
    std::mutex m_mutex;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> m_connections;
    std::atomic<int> m_up_count{0};
};

// ------------------------------------------------------- client (fixed) ---

// Same thread lifecycle as obs_plugin.cpp after the fix.
namespace fixed {

std::atomic<bool> shutting_down{false};
std::mutex lock;
ws_client websocket;
bool asio_initialized = false;
bool websocket_open = false;
websocketpp::connection_hdl connection_handle;
owned_thread websocket_thread;
owned_thread loop_thread;
std::string url;

std::atomic<int> opens{0}, closes{0}, fails{0}, sends{0}, self_disconnects{0};
std::atomic<int> loop_joined{0}, loop_self{0}, loop_timeout{0};
std::atomic<unsigned> handler_dice{0};

void stop_loop()
{
    switch (loop_thread.stop(2s)) {
    case stop_result::joined: ++loop_joined; break;
    case stop_result::detached_self: ++loop_self; break;
    case stop_result::detached_timeout: ++loop_timeout; break;
    case stop_result::not_running: break;
    }
}

void loop_function(std::stop_token stop)
{
    while (!shutting_down && !stop.stop_requested()) {
        if (!vortideck::lifecycle::sleep_for(stop, 20ms)) break;
        std::lock_guard l(lock);
        if (!websocket_open) continue;
        websocketpp::lib::error_code ec;
        websocket.send(connection_handle, "status", websocketpp::frame::opcode::text, ec);
        if (!ec) ++sends;
    }
}

void start_loop()
{
    if (shutting_down) return;
    loop_thread.start([](std::stop_token stop) { loop_function(stop); });
}

void disconnect()
{
    {
        std::lock_guard l(lock);
        websocket_open = false;
    }
    stop_loop();
    websocketpp::connection_hdl handle;
    {
        std::lock_guard l(lock);
        handle = connection_handle;
    }
    websocketpp::lib::error_code ec;
    websocket.close(handle, websocketpp::close::status::going_away, "disconnect", ec);
}

struct session {
    bool open()
    {
        websocketpp::lib::error_code ec;
        auto con = websocket.get_connection(url, ec);
        if (ec || !con) return false;
        con->set_open_handler([](websocketpp::connection_hdl) {
            {
                std::lock_guard l(lock);
                websocket_open = true;
            }
            ++opens;
            start_loop();
            // Like websocket_open_handler() when registration fails: a
            // disconnect() on the connection thread itself.
            if (handler_dice++ % 3 == 0) {
                ++self_disconnects;
                disconnect();
            }
        });
        con->set_close_handler([](websocketpp::connection_hdl) {
            {
                std::lock_guard l(lock);
                websocket_open = false;
            }
            ++closes;
        });
        con->set_fail_handler([](websocketpp::connection_hdl) {
            {
                std::lock_guard l(lock);
                websocket_open = false;
            }
            ++fails;
            stop_loop();  // websocket_fail_handler() does this on the connection thread
        });
        con->set_message_handler([](websocketpp::connection_hdl, ws_client::message_ptr) {});
        {
            std::lock_guard l(lock);
            connection_handle = con->get_handle();
        }
        websocket.connect(con);
        return true;
    }
    void run()
    {
        websocket.run();
        std::lock_guard l(lock);
        websocket_open = false;
    }
    void reset() { websocket.reset(); }
    void on_error(const std::string& message) { say("connection loop error: " + message); }
    std::chrono::milliseconds retry_delay(bool) const { return 30ms; }  // plugin: 1 s
};

bool connect()
{
    if (shutting_down) return false;
    std::lock_guard l(lock);
    if (!asio_initialized) {
        websocket.clear_access_channels(websocketpp::log::alevel::all);
        websocket.clear_error_channels(websocketpp::log::elevel::all);
        websocket.init_asio();
        websocket.set_reuse_addr(true);
        asio_initialized = true;
    }
    websocket_thread.start([](std::stop_token stop) {
        session s;
        vortideck::lifecycle::run_control_connection(stop, shutting_down, s);
    });
    return true;
}

void reconnect()
{
    disconnect();
    connect();
}

stop_result shutdown()
{
    shutting_down = true;
    websocket_thread.request_stop();
    {
        std::lock_guard l(lock);
        if (asio_initialized) websocket.stop();
    }
    return websocket_thread.stop(3s);
}

int run(int seconds)
{
    const auto port = pick_free_port();
    url = "ws://127.0.0.1:" + std::to_string(port);
    flapping_server server(port);
    server.start(60ms, 300ms);

    connect();

    std::atomic<bool> chaos_stop{false};
    std::atomic<int> chaos_calls{0};
    // A second thread that also starts and stops things, like the
    // websocket thread and the UI thread do in the plugin.
    std::thread helper([&] {
        std::mt19937 rng(42);
        while (!chaos_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 40));
            switch (rng() % 4) {
            case 0: disconnect(); break;
            case 1: start_loop(); break;
            case 2: stop_loop(); break;
            default: connect(); break;
            }
            ++chaos_calls;
        }
    });

    // The "UI thread": service dialog reconnects and disconnects.
    std::mt19937 rng(7);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 60));
        const auto before = std::chrono::steady_clock::now();
        if (rng() % 2) reconnect(); else disconnect();
        ++chaos_calls;
        const auto took = std::chrono::steady_clock::now() - before;
        if (took > 2500ms) fail("a UI-thread disconnect/reconnect blocked for " +
                                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms");
    }

    chaos_stop = true;
    helper.join();

    // OBS exit, in the EXIT handler's order.
    const auto shutdown_start = std::chrono::steady_clock::now();
    const auto result = shutdown();
    stop_loop();
    const auto shutdown_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - shutdown_start).count();
    server.stop();

    say("server up/down cycles: " + std::to_string(server.up_count()));
    say("client opens: " + std::to_string(opens) + ", closes: " + std::to_string(closes) +
        ", fails: " + std::to_string(fails) + ", status sends: " + std::to_string(sends));
    say("disconnects from inside the open handler: " + std::to_string(self_disconnects));
    say("chaos calls: " + std::to_string(chaos_calls));
    say("status loop stops: joined " + std::to_string(loop_joined) + ", from itself " + std::to_string(loop_self) +
        ", timed out " + std::to_string(loop_timeout));
    say(std::string("shutdown: connection thread ") + vortideck::lifecycle::to_string(result) + " in " +
        std::to_string(shutdown_ms) + " ms");

    if (result != stop_result::joined) fail("connection thread was not joined at shutdown");
    if (loop_thread.running()) fail("status loop still running after shutdown");
    if (loop_timeout != 0) fail("status loop did not stop in time");
    if (opens < 10) fail("too few successful connections; the reconnect loop is not working");
    if (self_disconnects == 0) fail("the open-handler disconnect path was not exercised");
    if (server.up_count() < 5) fail("server did not flap enough");
    say("PASS");
    return 0;
}

}  // namespace fixed

// ------------------------------------------------- client (pre-fix code) ---

// The v1.0.11 connection code, kept as close to obs_plugin.cpp as the test
// allows (logging and OBS calls removed).
namespace legacy {

std::atomic<bool> m_shutting_down{false};
std::mutex m_lock;
ws_client m_websocket;
bool m_websocket_open = false;
websocketpp::connection_hdl m_connection_handle;
std::unique_ptr<std::jthread> m_websocket_thread;
std::string url;
std::mutex m_compressor_ready_mutex;
std::condition_variable m_compressor_ready_cv;
bool disconnect_from_open_handler = false;
std::atomic<int> opens{0};

void disconnect();

ws_client::connection_ptr _create_connection()
{
    if (m_shutting_down) return nullptr;
    websocketpp::lib::error_code error_code;
    auto connection = m_websocket.get_connection(url, error_code);
    if (!connection || 0 != error_code.value()) return connection;
    connection->set_open_handler([](websocketpp::connection_hdl) {
        {
            std::lock_guard<std::mutex> wl(m_lock);
            m_websocket_open = true;
        }
        ++opens;
        m_compressor_ready_cv.notify_one();
        if (disconnect_from_open_handler) disconnect();  // "Failed to send registration message"
    });
    connection->set_close_handler([](websocketpp::connection_hdl) {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
    });
    m_connection_handle = connection->get_handle();
    return connection;
}

void _run_forever()
{
    std::set_terminate(on_terminate);  // test instrumentation (MSVC: per thread)
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket.clear_access_channels(websocketpp::log::alevel::all);
        m_websocket.clear_error_channels(websocketpp::log::elevel::all);
        m_websocket.init_asio();
        m_websocket.set_reuse_addr(true);
    }
    while (!m_shutting_down) {
        try {
            ws_client::connection_ptr connection = _create_connection();
            if (!connection) {
                for (int i = 0; i < 10 && !m_shutting_down; ++i) std::this_thread::sleep_for(100ms);
                continue;
            }
            m_websocket.connect(connection);
            try {
                m_websocket.run();
            } catch (const std::exception&) {
            }
            if (m_shutting_down) break;
            {
                std::lock_guard<std::mutex> wl(m_lock);
                m_websocket_open = false;
            }
            if (!m_shutting_down) m_websocket.reset();
            for (int i = 0; i < 10 && !m_shutting_down; ++i) std::this_thread::sleep_for(100ms);
        } catch (const std::exception&) {
            if (!m_shutting_down) m_websocket.reset();
            for (int i = 0; i < 10 && !m_shutting_down; ++i) std::this_thread::sleep_for(100ms);
        }
    }
}

void disconnect()
{
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
    }
    m_compressor_ready_cv.notify_all();
    if (m_websocket_thread) {
        try {
            if (m_shutting_down) {
                if (m_websocket_thread->joinable()) {
                    m_websocket_thread->request_stop();
                    m_websocket_thread->detach();
                }
                m_websocket_thread.reset();
                return;
            }
            m_websocket.stop();
            if (m_websocket_thread->joinable()) {
                say("legacy disconnect: requesting thread stop");
                m_websocket_thread->request_stop();
                std::atomic<bool> join_completed{false};
                std::jthread timeout_thread([&]() {
                    std::set_terminate(on_terminate);  // test instrumentation
                    m_websocket_thread->join();
                    join_completed = true;
                });
                auto timeout_start = std::chrono::steady_clock::now();
                while (!join_completed && (std::chrono::steady_clock::now() - timeout_start) < 3s) {
                    std::this_thread::sleep_for(100ms);
                }
                if (!join_completed) {
                    say("legacy disconnect: thread join timeout - forcing cleanup");
                    timeout_thread.detach();
                }
            }
            m_websocket.reset();
        } catch (const std::exception& e) {
            say(std::string("legacy disconnect: ") + e.what());
        }
        say("legacy disconnect: resetting the thread (~jthread joins)");
        m_websocket_thread.reset();
        say("legacy disconnect: done");
    }
}

bool connect()
{
    {
        std::lock_guard<std::mutex> wl(m_lock);
        m_websocket_open = false;
        if (nullptr == m_websocket_thread) m_websocket_thread.reset(new std::jthread(&_run_forever));
    }
    std::unique_lock<std::mutex> lk(m_compressor_ready_mutex);
    auto start_time = std::chrono::system_clock::now();
    while (!m_websocket_open && !m_shutting_down) {
        if (m_compressor_ready_cv.wait_for(lk, 100ms) == std::cv_status::timeout) {
            if (std::chrono::system_clock::now() - start_time > 3s) {
                say("legacy connect: connection timeout - disconnect()");
                lk.unlock();
                disconnect();
                return false;
            }
        }
    }
    return !m_shutting_down;
}

void obs_exit()
{
    say("legacy: OBS exit - shutdown flag set, websocket stopped");
    m_shutting_down = true;
    std::lock_guard<std::mutex> wl(m_lock);
    m_websocket.stop();
}

int run(const std::string& scenario)
{
    const auto port = pick_free_port();
    url = "ws://127.0.0.1:" + std::to_string(port);
    flapping_server server(port);

    if (scenario == "double-join") {
        // VortiDeck is not running when OBS starts: the startup thread's
        // connect() times out and calls disconnect(), which hangs.
        std::jthread([] {  // as obs_module_post_load()
            std::set_terminate(on_terminate);
            connect();
        }).detach();
        std::this_thread::sleep_for(8s);
        obs_exit();  // the connection thread now exits; both joiners wake
        std::this_thread::sleep_for(5s);
        say("no crash");
        return 0;
    }
    if (scenario == "self-join") {
        server.bring_up();
        disconnect_from_open_handler = true;
        {
            // Start only the connection thread, so the open handler's
            // disconnect() is the only one.
            std::lock_guard<std::mutex> wl(m_lock);
            m_websocket_thread.reset(new std::jthread(&_run_forever));
        }
        std::this_thread::sleep_for(10s);
        server.bring_down(false);
        say("no crash");
        return 0;
    }
    if (scenario == "hang") {
        server.bring_up();
        std::thread([] { connect(); }).detach();
        std::this_thread::sleep_for(1500ms);
        if (opens == 0) fail("legacy client did not connect");
        // The "UI thread" (service dialog accepted -> reconnect() -> disconnect()).
        std::atomic<bool> returned{false};
        std::thread ui([&] {
            disconnect();
            returned = true;
        });
        ui.detach();
        for (int i = 0; i < 100 && !returned; ++i) std::this_thread::sleep_for(100ms);
        if (!returned) {
            say("HANG: disconnect() on the UI thread has not returned after 10 s (OBS: Application Hang)");
            std::_Exit(4);
        }
        say("no hang");
        return 0;
    }
    fail("unknown legacy scenario " + scenario);
}

}  // namespace legacy

}  // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::set_terminate(on_terminate);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    int seconds = 8;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--legacy" && i + 1 < argc) return legacy::run(argv[++i]);
        if (arg == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
    }

    // Watchdog: a hang is a failure, not a ctest timeout.
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds + 20));
        say("FAIL: HANG - the test did not finish");
        std::_Exit(2);
    }).detach();

    return fixed::run(seconds);
}
