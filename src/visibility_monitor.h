#pragma once

#include <obs.h>
#include <atomic>
#include <unordered_map>
#include <mutex>

class VisibilityMonitor {
public:
    VisibilityMonitor();
    ~VisibilityMonitor();
    
    // Start monitoring a source
    void add_source(obs_source_t* source, const std::string& id);
    void remove_source(const std::string& id);
    
    // Check visibility without polling
    bool is_visible(const std::string& id) const;
    
    // Callbacks for visibility changes
    using visibility_callback_t = std::function<void(const std::string& id, bool visible)>;
    void set_visibility_callback(visibility_callback_t callback);
    
private:
    struct SourceInfo {
        obs_source_t* source;
        std::atomic<bool> visible;
        signal_handler_t* signal_handler;
    };
    
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, SourceInfo> m_sources;
    visibility_callback_t m_visibility_callback;
    
    // Signal handlers
    static void on_show(void* data, calldata_t* cd);
    static void on_hide(void* data, calldata_t* cd);
    static void on_activate(void* data, calldata_t* cd);
    static void on_deactivate(void* data, calldata_t* cd);
    
    // Scene item visibility handler
    static void on_item_visible(void* data, calldata_t* cd);
    
    void connect_source_signals(const std::string& id, obs_source_t* source);
    void disconnect_source_signals(const std::string& id);
};