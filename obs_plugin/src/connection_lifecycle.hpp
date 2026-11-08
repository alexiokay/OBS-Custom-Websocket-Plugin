#pragma once

// Thread ownership for the VortiDeck control connection.
//
// Rules this header enforces (see obs_plugin/docs/THREAD_OWNERSHIP.md):
//  - every worker thread has exactly one owner object, and only the owner
//    starts, stops and joins it;
//  - a thread is never joined from itself (that throws
//    resource_deadlock_would_occur, which std::terminate()s inside a
//    noexcept destructor);
//  - a joinable thread is never destroyed while it runs, and it is never
//    joined by two threads at once;
//  - workers sleep on their stop_token, so a stop request ends them promptly;
//  - no exception escapes a thread body (that would std::terminate OBS).
//
// The header is pure C++ so the lifecycle can be stress-tested without OBS.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace vortideck::lifecycle {

// Sleeps for `duration` unless a stop is requested first.
// Returns true when the full time elapsed, false when stop was requested.
inline bool sleep_for(std::stop_token stop, std::chrono::milliseconds duration)
{
    if (stop.stop_requested()) return false;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_lock lock(mutex);
    cv.wait_for(lock, stop, duration, [] { return false; });
    return !stop.stop_requested();
}

enum class stop_result {
    not_running,       // nothing to stop
    joined,            // the thread finished and was joined
    detached_self,     // stop() ran on the thread itself; it exits on its own
    detached_timeout,  // the thread did not finish in time and was released
};

inline const char* to_string(stop_result result)
{
    switch (result) {
    case stop_result::not_running: return "not running";
    case stop_result::joined: return "joined";
    case stop_result::detached_self: return "released (stop called from the thread itself)";
    case stop_result::detached_timeout: return "released (did not finish in time)";
    }
    return "unknown";
}

// One worker thread with one owner. Start, stop and join go through this
// object only, from any thread.
class owned_thread {
public:
    owned_thread() = default;
    owned_thread(const owned_thread&) = delete;
    owned_thread& operator=(const owned_thread&) = delete;

    // Runs while the module is unloaded (possibly under the loader lock), so
    // it must neither block nor terminate. Owners stop their threads earlier.
    ~owned_thread()
    {
        std::lock_guard lock(m_mutex);
        if (m_thread.joinable()) {
            m_thread.request_stop();
            m_thread.detach();
        }
    }

    // Starts `body(std::stop_token)` unless a previous run is still active.
    // A previous run that already finished is joined first.
    template <class Body>
    bool start(Body&& body)
    {
        std::lock_guard lock(m_mutex);
        if (m_thread.joinable()) {
            if (!m_exit->finished()) return false;
            m_thread.join();  // finished: returns at once
        }
        auto exit = std::make_shared<exit_signal>();
        m_exit = exit;
        m_thread = std::jthread([exit, body = std::forward<Body>(body)](std::stop_token stop) mutable {
            struct mark_finished {
                exit_signal& signal;
                ~mark_finished() { signal.set(); }
            } guard{*exit};
            try {
                body(stop);
            } catch (...) {
                // Bodies log their own errors. Swallowing here keeps an
                // unexpected exception from terminating OBS.
            }
        });
        return true;
    }

    bool running() const
    {
        std::lock_guard lock(m_mutex);
        return m_thread.joinable() && !m_exit->finished();
    }

    bool is_current_thread() const
    {
        std::lock_guard lock(m_mutex);
        return m_thread.joinable() && m_thread.get_id() == std::this_thread::get_id();
    }

    void request_stop()
    {
        std::lock_guard lock(m_mutex);
        if (m_thread.joinable()) m_thread.request_stop();
    }

    // Requests stop and waits up to `timeout` for the thread to finish, then
    // joins it. Safe from any thread, including the worker itself, and safe
    // to call concurrently: exactly one caller takes the thread.
    stop_result stop(std::chrono::milliseconds timeout)
    {
        std::jthread thread;
        std::shared_ptr<exit_signal> exit;
        {
            std::lock_guard lock(m_mutex);
            if (!m_thread.joinable()) return stop_result::not_running;
            thread = std::move(m_thread);
            exit = std::move(m_exit);
        }
        thread.request_stop();
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
            return stop_result::detached_self;
        }
        if (!exit->wait_for(timeout)) {
            thread.detach();
            return stop_result::detached_timeout;
        }
        thread.join();
        return stop_result::joined;
    }

private:
    struct exit_signal {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;

        void set()
        {
            {
                std::lock_guard lock(mutex);
                done = true;
            }
            cv.notify_all();
        }
        bool finished()
        {
            std::lock_guard lock(mutex);
            return done;
        }
        bool wait_for(std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, timeout, [this] { return done; });
        }
    };

    mutable std::mutex m_mutex;
    std::jthread m_thread;
    std::shared_ptr<exit_signal> m_exit;
};

// The control-connection supervisor loop. It runs on one thread for the whole
// session: open a connection, run its I/O until the connection ends, reset,
// wait, repeat. Other threads never stop or join it to drop a connection;
// they close the connection and this loop reconnects.
//
// Session needs:
//   bool open();            // create and start a connection; false = retry later
//   void run();             // run I/O until the connection ends or I/O is stopped
//   void reset();           // make I/O runnable again after run() returned
//   void on_error(const std::string&);
//   std::chrono::milliseconds retry_delay(bool opened);
//
// Shutdown order: set `shutting_down`, request stop on the thread, stop I/O,
// then stop/join the thread. reset() happens before the stop check, so an I/O
// stop that lands while this loop is between connections is never undone.
template <class Session>
void run_control_connection(std::stop_token stop, const std::atomic<bool>& shutting_down, Session& session)
{
    const auto stopping = [&] { return stop.stop_requested() || shutting_down.load(); };
    while (!stopping()) {
        bool opened = false;
        try {
            opened = session.open();
            if (opened && !stopping()) session.run();
        } catch (const std::exception& e) {
            session.on_error(e.what());
        } catch (...) {
            session.on_error("unknown exception");
        }
        if (stopping()) break;

        if (opened) {
            try {
                session.reset();
            } catch (const std::exception& e) {
                session.on_error(e.what());
            } catch (...) {
                session.on_error("unknown exception during reset");
            }
        }
        sleep_for(stop, session.retry_delay(opened));
    }
}

}  // namespace vortideck::lifecycle
