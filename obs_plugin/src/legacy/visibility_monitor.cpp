#include "visibility_monitor.h"
#include <obs-module.h>

VisibilityMonitor::VisibilityMonitor() {}

VisibilityMonitor::~VisibilityMonitor() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, info] : m_sources) {
        disconnect_source_signals(id);
    }
}

void VisibilityMonitor::add_source(obs_source_t* source, const std::string& id) {
    if (!source) return;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Remove existing if any
    auto it = m_sources.find(id);
    if (it != m_sources.end()) {
        disconnect_source_signals(id);
    }
    
    // Add new source
    SourceInfo info;
    info.source = source;
    info.visible = obs_source_active(source);
    info.signal_handler = obs_source_get_signal_handler(source);
    
    m_sources[id] = info;
    connect_source_signals(id, source);
}

void VisibilityMonitor::remove_source(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(id);
    if (it != m_sources.end()) {
        disconnect_source_signals(id);
        m_sources.erase(it);
    }
}

bool VisibilityMonitor::is_visible(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(id);
    if (it != m_sources.end()) {
        return it->second.visible.load();
    }
    return false;
}

void VisibilityMonitor::set_visibility_callback(visibility_callback_t callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_visibility_callback = callback;
}

void VisibilityMonitor::connect_source_signals(const std::string& id, obs_source_t* source) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || !it->second.signal_handler) return;
    
    signal_handler_t* handler = it->second.signal_handler;
    
    // Use a wrapper struct to pass both monitor and id
    struct callback_data {
        VisibilityMonitor* monitor;
        std::string source_id;
    };
    
    auto* data = new callback_data{this, id};
    
    // Connect to source visibility signals
    // show/hide: visible on any display
    signal_handler_connect(handler, "show", on_show, data);
    signal_handler_connect(handler, "hide", on_hide, data);
    
    // activate/deactivate: visible on stream/recording (main view)
    signal_handler_connect(handler, "activate", on_activate, data);
    signal_handler_connect(handler, "deactivate", on_deactivate, data);
}

void VisibilityMonitor::disconnect_source_signals(const std::string& id) {
    auto it = m_sources.find(id);
    if (it == m_sources.end() || !it->second.signal_handler) return;
    
    signal_handler_t* handler = it->second.signal_handler;
    
    // Disconnect all signals - need to pass the same data pointer used in connect
    // In practice, OBS disconnects by function pointer, so nullptr works
    signal_handler_disconnect(handler, "show", on_show, this);
    signal_handler_disconnect(handler, "hide", on_hide, this);
    signal_handler_disconnect(handler, "activate", on_activate, this);
    signal_handler_disconnect(handler, "deactivate", on_deactivate, this);
}

// Signal handlers
void VisibilityMonitor::on_show(void* data, calldata_t* cd) {
    if (!data) return;
    auto* cb_data = static_cast<callback_data*>(data);
    
    std::lock_guard<std::mutex> lock(cb_data->monitor->m_mutex);
    auto it = cb_data->monitor->m_sources.find(cb_data->source_id);
    if (it != cb_data->monitor->m_sources.end()) {
        bool was_visible = it->second.visible.exchange(true);
        if (!was_visible && cb_data->monitor->m_visibility_callback) {
            cb_data->monitor->m_visibility_callback(cb_data->source_id, true);
        }
    }
}

void VisibilityMonitor::on_hide(void* data, calldata_t* cd) {
    if (!data) return;
    auto* cb_data = static_cast<callback_data*>(data);
    
    std::lock_guard<std::mutex> lock(cb_data->monitor->m_mutex);
    auto it = cb_data->monitor->m_sources.find(cb_data->source_id);
    if (it != cb_data->monitor->m_sources.end()) {
        bool was_visible = it->second.visible.exchange(false);
        if (was_visible && cb_data->monitor->m_visibility_callback) {
            cb_data->monitor->m_visibility_callback(cb_data->source_id, false);
        }
    }
}

void VisibilityMonitor::on_activate(void* data, calldata_t* cd) {
    on_show(data, cd); // Same behavior
}

void VisibilityMonitor::on_deactivate(void* data, calldata_t* cd) {
    on_hide(data, cd); // Same behavior
}

// For scene items specifically
void VisibilityMonitor::on_item_visible(void* data, calldata_t* cd) {
    if (!data || !cd) return;
    
    bool visible = calldata_bool(cd, "visible");
    obs_sceneitem_t* item = nullptr;
    calldata_get_ptr(cd, "item", &item);
    
    if (!item) return;
    
    obs_source_t* source = obs_sceneitem_get_source(item);
    if (!source) return;
    
    // Find source by checking all registered sources
    auto* cb_data = static_cast<callback_data*>(data);
    std::lock_guard<std::mutex> lock(cb_data->monitor->m_mutex);
    
    for (auto& [id, info] : cb_data->monitor->m_sources) {
        if (info.source == source) {
            bool was_visible = info.visible.exchange(visible);
            if (was_visible != visible && cb_data->monitor->m_visibility_callback) {
                cb_data->monitor->m_visibility_callback(id, visible);
            }
            break;
        }
    }
}