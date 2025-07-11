#include "banner_manager.hpp"
#include <UI/obs-frontend-api/obs-frontend-api.h>
#include <libobs/obs-module.h>
#include <libobs/util/platform.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <string>
#include <set>
#include <nlohmann/json.hpp>

using namespace vorti::applets;

// Forward declarations
extern void log_to_obs(std::string_view message);

// Forward declaration for plugin interface
struct plugin_interface {
    bool is_connected() const;
    bool send_message(const nlohmann::json& message) const;
};

// Global plugin instance pointer
extern plugin_interface* g_obs_plugin_instance;

banner_manager::banner_manager()
    : m_banner_source(nullptr)
    , m_banner_visible(false)
    , m_banner_persistent(false)
    , m_persistence_monitor_active(false)
    , m_current_banner_content("")
    , m_current_content_type("")
    , m_banner_source_name("VortiDeck Banner")
{
    log_message("CONSTRUCTOR: Initializing banner manager...");
    
    // Initialize timestamps
    m_last_premium_update = std::chrono::system_clock::now();
    m_last_ad_end_time = std::chrono::system_clock::now() - std::chrono::minutes(2); // Allow immediate first ad
    
    // CRITICAL: Initialize FREE USER mode as DEFAULT
    m_is_premium.store(false);  // FREE USER by default
    m_revenue_share.store(0.05f);  // 5% revenue share for free users
    m_custom_positioning.store(false);  // No custom positioning for free users
    m_ad_frequency_minutes.store(5);  // 5 minutes ad frequency for free users
    
    log_message("CONSTRUCTOR: Banner manager initialized - FREE USER MODE (5% revenue share)");
    log_message("CONSTRUCTOR: FREE USER - FORCED banner system - banners MUST be present in ALL scenes");
    log_message("CONSTRUCTOR: FREE USER - Limited banner control - upgrade to premium for complete banner freedom");
    log_message("CONSTRUCTOR: FREE USER - Banners auto-restore after 10sec hiding, locked positioning");
    log_message("CONSTRUCTOR: FREE USER - Enhanced protection system ACTIVE - banners protected from removal");
    
    // Basic OBS functionality tests
    log_message("CONSTRUCTOR: Testing basic OBS functionality...");
    
    // Test if OBS core functions are available
    try {
        // Test obs_get_version_string if available
        const char* obs_version = obs_get_version_string();
        if (obs_version) {
            log_message("CONSTRUCTOR: OBS version: " + std::string(obs_version));
        } else {
            log_message("CONSTRUCTOR: Could not get OBS version (function may not be available)");
        }
    } catch (...) {
        log_message("CONSTRUCTOR: Exception while testing OBS version function");
    }
    
    // Test if we can create a basic color source (this is a good test of OBS functionality)
    try {
        obs_data_t* test_settings = obs_data_create();
        obs_data_set_int(test_settings, "color", 0xFF0000FF);
        obs_data_set_int(test_settings, "width", 100);
        obs_data_set_int(test_settings, "height", 100);
        
        obs_source_t* test_source = obs_source_create("color_source", "VortiDeck_Test_Source", test_settings, nullptr);
        obs_data_release(test_settings);
        
        if (test_source) {
            log_message("CONSTRUCTOR: OBS source creation test PASSED - color_source works");
            obs_source_release(test_source);
        } else {
            log_message("CONSTRUCTOR: OBS source creation test FAILED - color_source creation failed");
        }
    } catch (...) {
        log_message("CONSTRUCTOR: Exception during OBS source creation test");
    }
    
    // DON'T create sources immediately - OBS isn't fully loaded yet!
    // We'll delay initialization until OBS is ready
    log_message("CONSTRUCTOR: Banner initialization will start after OBS is fully loaded");
}

banner_manager::~banner_manager()
{
    log_message("Banner manager shutting down - stopping all threads...");
    
    // Stop all background threads FIRST
    stop_ad_rotation();        // Stop ad rotation thread
    stop_duration_timer();     // Stop duration timer thread
    stop_window_gap();         // Stop window gap thread
    
    // Stop analytics if running
    if (m_analytics_reporting_active.load()) {
        m_analytics_reporting_active.store(false);
        m_stop_analytics.store(true);
        if (m_analytics_thread.joinable()) {
            m_analytics_thread.join();
        }
    }
    
    // Clean up signal handlers and monitoring
    disconnect_scene_signals();
    stop_persistence_monitor();
    
    // Clean up OBS sources
    if (m_banner_source) {
        obs_source_release(m_banner_source);
        m_banner_source = nullptr;
    }
    
    log_message("Banner manager destroyed - all threads stopped");
}

void banner_manager::shutdown()
{
    log_message("Banner manager shutdown requested - stopping all threads...");
    
    // Set shutdown flags to prevent new operations
    m_shutting_down = true;
    
    // Stop all background threads
    stop_ad_rotation();        // Stop ad rotation thread
    stop_duration_timer();     // Stop duration timer thread
    stop_window_gap();         // Stop window gap thread
    
    // Stop analytics if running
    if (m_analytics_reporting_active.load()) {
        m_analytics_reporting_active.store(false);
        m_stop_analytics.store(true);
        if (m_analytics_thread.joinable()) {
            m_analytics_thread.join();
        }
    }
    
    // Clean up signal handlers and monitoring
    disconnect_scene_signals();
    stop_persistence_monitor();
    
    log_message("Banner manager shutdown complete - all threads stopped");
}

void banner_manager::initialize_after_obs_ready()
{
    log_message("INITIALIZATION: Starting banner initialization process...");
    
    // CRITICAL: Prevent multiple initialization calls
    if (m_initialization_started.load()) {
        log_message("INITIALIZATION: Already started - ignoring duplicate call");
        return;
    }
    
    // Safety check - ensure OBS is fully initialized
    if (!obs_frontend_get_current_scene()) {
        log_message("INITIALIZATION: OBS not fully initialized yet - delaying banner initialization");
        
        // Try again after a short delay
        std::jthread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            initialize_after_obs_ready();
        }).detach();
        return;
    }
    
    log_message("INITIALIZATION: OBS is ready, proceeding with banner initialization");
    
    // Set flag to prevent multiple calls
    m_initialization_started.store(true);
    
    log_message("INITIALIZATION: User type: " + PremiumStatusHandler::get_user_type_string(this));
    
    if (!PremiumStatusHandler::is_premium(this)) {
        // FREE USERS: Initialize banners after OBS is fully loaded
        log_message("INITIALIZATION: FREE USER - Starting automatic banner initialization");
        
        // Create default transparent banner content for FREE USERS
        log_message("INITIALIZATION: Creating default transparent banner content");
        set_banner_content("transparent", "transparent");  // Transparent banner
        
        // Verify banner source was created
        if (m_banner_source) {
            log_message("INITIALIZATION: Banner source created successfully");
        } else {
            log_message("INITIALIZATION: ERROR - Banner source creation failed!");
        }
        
        // Force show banners across ALL scenes for FREE USERS
        log_message("INITIALIZATION: Forcing banner display across all scenes");
        show_banner();
        
        log_message("INITIALIZATION: FREE USER - Automatic banner initialization complete");
    } else {
        // PREMIUM USERS: Just log that they have freedom
        log_message("INITIALIZATION: PREMIUM USER - Complete banner freedom (no automatic banners)");
    }
    
    // Enable signal connections for free users after OBS is fully ready
    if (!PremiumStatusHandler::is_premium(this)) {
        log_message("INITIALIZATION: Enabling signal-based banner protection for free users");
        enable_signal_connections_when_safe();
    }
    
    log_message("INITIALIZATION: Banner initialization process completed");
}

void banner_manager::enable_signal_connections_when_safe()
{
    // CRITICAL: Prevent multiple signal connections
    if (m_signals_connected.load()) {
        log_message("Signal connections already established - ignoring duplicate call");
        return;
    }
    
    // STEP 1: Re-enable signal connections with minimal handlers
    if (!PremiumStatusHandler::is_premium(this)) {
        log_message("STEP 1: Enabling signal connections with minimal handlers");
        connect_scene_signals();
        m_signals_connected.store(true);
    } else {
        PremiumStatusHandler::log_premium_action(this, "signal connections", "not needed - complete banner freedom");
    }
}

void banner_manager::connect_scene_signals()
{
    log_message("Connecting scene signals for banner enforcement");
    
    // Connect to all existing scenes
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    if (!scenes.sources.array) {
        log_message("WARNING: No scenes available for signal connection");
        return;
    }
    
    int connected_scenes = 0;
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* scene_source = scenes.sources.array[i];
        if (!scene_source) continue;
        
        signal_handler_t* handler = obs_source_get_signal_handler(scene_source);
        if (handler) {
            // Connect all three signal handlers
            signal_handler_connect(handler, "item_remove", on_item_remove, this);
            signal_handler_connect(handler, "item_visible", on_item_visible, this);
            signal_handler_connect(handler, "item_transform", on_item_transform, this);
            connected_scenes++;
        }
    }
    
    obs_frontend_source_list_free(&scenes);
    log_message("Scene signals connected for " + std::to_string(connected_scenes) + " scenes - banner enforcement ACTIVE");
}

void banner_manager::disconnect_scene_signals()
{
    log_message("Disconnecting scene signals");
    
    try {
    // Disconnect from all existing scenes
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
        if (!scenes.sources.array) {
            log_message("WARNING: No scenes available for signal disconnection");
            m_signals_connected.store(false);  // Reset flag even if no scenes
            return;
        }
        
        int disconnected_scenes = 0;
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* scene_source = scenes.sources.array[i];
            if (!scene_source) continue;
        
            signal_handler_t* handler = obs_source_get_signal_handler(scene_source);
        if (handler) {
                // Disconnect all signal handlers
            signal_handler_disconnect(handler, "item_remove", on_item_remove, this);
            signal_handler_disconnect(handler, "item_visible", on_item_visible, this);
            signal_handler_disconnect(handler, "item_transform", on_item_transform, this);
                disconnected_scenes++;
        }
    }
    
    obs_frontend_source_list_free(&scenes);
        log_message("Scene signals disconnected from " + std::to_string(disconnected_scenes) + " scenes");
        
        // Reset flag after disconnection
        m_signals_connected.store(false);
        
    } catch (...) {
        log_message("ERROR: Exception during signal disconnection - ignoring");
        m_signals_connected.store(false);  // Reset flag even if exception
    }
}

// REMOVED: Legacy minimal signal handlers - no longer needed

void banner_manager::on_item_remove(void* data, calldata_t* calldata)
{
    if (!data || !calldata) return;
    
    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;
    
    // CRITICAL: Skip if intentional hide is in progress
    if (manager->m_intentional_hide_in_progress.load()) {
        return;  // Don't interfere with intentional hide operations
    }
    
    try {
        obs_sceneitem_t* item = (obs_sceneitem_t*)calldata_ptr(calldata, "item");
        if (!item) return;
    
        obs_source_t* source = obs_sceneitem_get_source(item);
        if (!source) return;
    
        const char* name = obs_source_get_name(source);
        if (!name) return;
        
        // Check if this is our banner being removed (metadata-based detection)
        if (manager->is_vortideck_banner_item(item)) {
            manager->log_message("SIGNAL: Banner removal detected - Name: " + std::string(name) + 
                               ", User: " + PremiumStatusHandler::get_user_type_string(manager));
            
            if (!PremiumStatusHandler::is_premium(manager)) {
                manager->log_message("SIGNAL: FREE USER - Starting banner restoration");
                
                std::jthread([manager]() {
                    manager->log_message("SIGNAL: Restoration jthread started");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    
                    // Check again if intentional hide is still in progress
                    if (!manager->m_intentional_hide_in_progress.load()) {
                        manager->log_message("SIGNAL: Calling enforce_banner_visibility");
                        manager->enforce_banner_visibility();
                        
                        // Also trigger cleanup for free users after restoration
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        manager->cleanup_banner_names_after_deletion();
                    } else {
                        manager->log_message("SIGNAL: Skipping restoration - intentional hide in progress");
                    }
                    
                    manager->log_message("SIGNAL: Restoration jthread completed");
                }).detach();
            } else {
                // PREMIUM USER: Trigger cleanup after banner deletion
                PremiumStatusHandler::log_premium_action(manager, "banner deletion", "triggering cleanup");
                
                // Trigger cleanup in a jthread to avoid blocking the signal
                std::jthread([manager]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    manager->cleanup_banner_names_after_deletion();
                }).detach();
            }
        }
        
    } catch (...) {
        manager->log_message("SIGNAL: Exception in item_remove detection");
    }
}

void banner_manager::on_item_visible(void* data, calldata_t* calldata)
{
    if (!data || !calldata) return;
    
    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;
    
    // CRITICAL: Skip if intentional hide is in progress
    if (manager->m_intentional_hide_in_progress.load()) {
        return;  // Don't interfere with intentional hide operations
    }
    
    try {
        obs_sceneitem_t* item = (obs_sceneitem_t*)calldata_ptr(calldata, "item");
        if (!item) return;
    
        bool visible = calldata_bool(calldata, "visible");
    
        obs_source_t* source = obs_sceneitem_get_source(item);
        if (!source) return;
    
        const char* name = obs_source_get_name(source);
        if (!name) return;
        
        // Check if this is our banner visibility change (metadata-based detection)
        if (manager->is_vortideck_banner_item(item)) {
            bool is_premium = manager->m_is_premium.load();
            
            // Only log significant changes to reduce spam
            if (!visible) {
                manager->log_message("SIGNAL: Banner hidden detected - Name: " + std::string(name) + 
                                   ", User: " + (is_premium ? "premium" : "free"));
            }
            
            if (!is_premium && !visible) {
                manager->log_message("SIGNAL: FREE USER - Immediate banner restoration (jthread-based)");
                
                // jthread-based restoration for free users - more reliable than direct call
                std::jthread([manager, item, name]() {
                    try {
                        // Small delay to ensure the visibility change is complete
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        
                        // Check again if intentional hide is still in progress
                        if (!manager->m_intentional_hide_in_progress.load()) {
                            obs_sceneitem_set_visible(item, true);
                            manager->log_message("SIGNAL: Banner visibility restored - Name: " + std::string(name));
                        }
                    } catch (...) {
                        manager->log_message("SIGNAL: Exception in visibility restoration jthread");
                    }
                }).detach();
            }
        }
        
    } catch (...) {
        manager->log_message("SIGNAL: Exception in item_visible handler");
    }
}

void banner_manager::on_item_transform(void* data, calldata_t* calldata)
{
    // STEP 5: FIXED - Prevent infinite loop with correction flag
    if (!data || !calldata) return;
    
    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;
    
    // CRITICAL: Skip if we're currently correcting position (prevents infinite loop)
    if (manager->m_correcting_position.load()) {
        return;
    }
    
    try {
    obs_sceneitem_t* item = (obs_sceneitem_t*)calldata_ptr(calldata, "item");
    if (!item) return;
    
    obs_source_t* source = obs_sceneitem_get_source(item);
    if (!source) return;
    
    const char* name = obs_source_get_name(source);
    if (!name) return;
    
        // Check if this is our banner being moved/resized (metadata-based detection)
        if (manager->is_vortideck_banner_item(item)) {
        bool is_premium = manager->m_is_premium.load();
        if (!is_premium) {
                // STEP 5: Check if position actually needs correction (avoid spam)
                vec2 current_pos;
                obs_sceneitem_get_pos(item, &current_pos);
                
                vec2 target_pos;
                target_pos.x = 0;
                target_pos.y = 0; // 0,0 for full-screen positioning
                
                // Only correct if position is actually wrong (tolerance of 5 pixels)
                bool needs_correction = (fabs(current_pos.x - target_pos.x) > 5) || 
                                       (fabs(current_pos.y - target_pos.y) > 5);
                
                if (needs_correction) {
                    manager->log_message("STEP 5: FREE USER - Banner moved/resized! IMMEDIATE position restoration");
                    
                    // STEP 5: Set flag to prevent infinite loop
                    manager->m_correcting_position.store(true);
                    
                    // STEP 5: IMMEDIATE position restoration for free users
                    obs_sceneitem_set_pos(item, &target_pos);
                    
            vec2 scale;
            scale.x = 1.0f;
            scale.y = 1.0f;
            obs_sceneitem_set_scale(item, &scale);
            
                    // STEP 5: Clear flag after corrections are done
                    manager->m_correcting_position.store(false);
                    
                    manager->log_message("STEP 5: Banner position/size IMMEDIATELY restored - free users cannot move banners!");
                }
            }
        }
        
    } catch (...) {
        manager->log_message("STEP 5: Exception in item_transform handler");
        // STEP 5: Ensure flag is cleared even if exception occurs
        manager->m_correcting_position.store(false);
    }
}

void banner_manager::cleanup_for_scene_collection_change()
{
    log_message("Scene collection changing - cleaning up banner sources");
    
    // Stop monitoring to prevent interference
    stop_persistence_monitor();
    
    // Remove banner from all scenes
    remove_banner_from_scenes();
    
    // Release banner source to prevent "sources could not be unloaded" error
    if (m_banner_source) {
        obs_source_release(m_banner_source);
        m_banner_source = nullptr;
    }
    
    // Clear content so it can be recreated after scene collection change
    m_banner_visible = false;
    
    log_message("Banner sources cleaned up for scene collection change");
}

void banner_manager::show_banner(bool enable_duration_timer)
{
    log_message("SHOW_BANNER: Starting banner display process...");
    
    log_message("SHOW_BANNER: User type: " + PremiumStatusHandler::get_user_type_string(this));
    
    // CHECK AD FREQUENCY LIMITS (free users only)
    if (!PremiumStatusHandler::is_premium(this) && !can_show_ad_now()) {
        auto now = std::chrono::system_clock::now();
        auto time_since_last = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_ad_end_time);
        int remaining_wait = 60 - static_cast<int>(time_since_last.count());
        
        log_message("SHOW_BANNER: FREE USER - Ad frequency limit exceeded. Must wait " + 
                   std::to_string(remaining_wait) + " more seconds before next ad.");
        return; // Don't show the banner yet
    }
    
    // STOP ANY EXISTING DURATION TIMER
    stop_duration_timer();
    
    // START WINDOW MANAGEMENT (if not already active)
    if (!m_window_active.load()) {
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        start_ad_window("banner_" + std::to_string(timestamp));
        log_message("SHOW_BANNER: Started ad window for banner display");
    }
    
    if (!m_banner_source) {
        log_message("SHOW_BANNER: No banner source exists, creating one...");
        
        if (!m_current_banner_content.empty()) {
            log_message("SHOW_BANNER: Using existing content: " + m_current_content_type + " - " + m_current_banner_content.substr(0, 50) + "...");
            create_banner_source(m_current_banner_content, m_current_content_type);
        } else {
            log_message("SHOW_BANNER: No banner content set - creating transparent placeholder");
            set_banner_content("transparent", "transparent");  // Create transparent banner
        }
        
        // Verify banner source creation
        if (m_banner_source) {
            log_message("SHOW_BANNER: Banner source created successfully");
        } else {
            log_message("SHOW_BANNER: ERROR - Failed to create banner source!");
            return;
        }
    } else {
        log_message("SHOW_BANNER: Using existing banner source");
    }
    
    if (!PremiumStatusHandler::is_premium(this)) {
        // FREE USERS: FORCED banner initialization across ALL scenes
        PremiumStatusHandler::log_premium_action(this, "banner display", "forcing across all scenes");
        initialize_banners_all_scenes();
        m_banner_visible = true;
        
        log_message("SHOW_BANNER: FREE USER - Banner display complete with restrictions");
        log_message("SHOW_BANNER: FREE USER - Limited hiding (5sec auto-restore), no positioning control");
        
        // For free users, automatically start enhanced monitoring (ALWAYS)
        if (!m_persistence_monitor_active) {
            log_message("SHOW_BANNER: FREE USER - Starting banner protection monitoring");
            start_persistence_monitor();
        }
        
        // FORCE immediate enforcement for FREE USERS
        log_message("SHOW_BANNER: FREE USER - Enforcing banner visibility");
        enforce_banner_visibility();
        
        // ADD DURATION CONTROL: Auto-hide after time limit for free users (if requested)
        if (enable_duration_timer) {
            int custom_duration = m_custom_banner_duration.load();
            int max_duration = custom_duration > 0 ? 
                              std::min(custom_duration, get_max_duration_for_user_type()) : 
                              get_max_duration_for_user_type(); // Respect user type limits
            log_message("SHOW_BANNER: FREE USER - Starting duration timer for " + std::to_string(max_duration) + 
                       " seconds" + (custom_duration > 0 ? " (custom duration)" : " (default)"));
            
            start_duration_timer(max_duration);
        } else {
            log_message("SHOW_BANNER: FREE USER - Duration timer skipped (managed externally)");
        }
    } else {
        // PREMIUM USERS: Complete freedom - NO forced banner initialization
        PremiumStatusHandler::log_premium_action(this, "banner display", "complete freedom mode (no forced banners)");
        log_message("SHOW_BANNER: PREMIUM USER - Use WebSocket API to add banners if desired");
        // NO automatic banner creation for premium users
        // They have complete choice whether to have banners or not
        
        // But if they do show a banner, apply premium duration limits (if requested)
        if (enable_duration_timer && m_banner_source && m_banner_visible) {
            int custom_duration = m_custom_banner_duration.load();
            int max_duration = custom_duration > 0 ? 
                              std::min(custom_duration, get_max_duration_for_user_type()) : 
                              get_max_duration_for_user_type(); // Premium users get longer limits
            log_message("SHOW_BANNER: PREMIUM USER - Starting duration timer for " + std::to_string(max_duration) + 
                       " seconds" + (custom_duration > 0 ? " (custom duration)" : " (default)"));
            
            start_duration_timer(max_duration);
        } else if (!enable_duration_timer) {
            log_message("SHOW_BANNER: PREMIUM USER - Duration timer skipped (managed externally)");
        }
    }
    
    log_message("SHOW_BANNER: Banner display process completed");
}

void banner_manager::hide_banner()
{
    log_message("HIDE_BANNER: Starting banner hide process...");
    
    // Check premium status - free users have VERY limited hiding privileges
    if (!PremiumStatusHandler::handle_premium_restriction(this, "banner_hide", "banner hiding")) {
        log_message("HIDE_BANNER: FREE USER - Banner hiding heavily restricted - upgrade to premium for full control");
        log_message("HIDE_BANNER: FREE USER - Banners can only be hidden for 5 seconds before auto-restore");
        
        // Set flag to prevent signal handlers from interfering during intentional hide
        m_intentional_hide_in_progress.store(true);
        log_message("HIDE_BANNER: FREE USER - Signal protection enabled during intentional hide");
        
        // Allow VERY temporary hiding but schedule auto-restore for free users
        remove_banner_from_scenes();
        
        // Schedule auto-restore in only 5 seconds for free users (very limited hiding)
        // BUT don't auto-restore if this is a duration timer hide (permanent until next ad)
        std::jthread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!PremiumStatusHandler::is_premium(this)) {
                // Only auto-restore if this wasn't a duration timer hide (check if we can show ads)
                if (can_show_ad_now()) {
                    log_message("HIDE_BANNER: FREE USER - Auto-restoring banner after 5 seconds");
                    show_banner();
                    
                    // CRITICAL: Re-enable full protection system for free users
                    log_message("HIDE_BANNER: FREE USER - Re-enabling signal-based protection system");
                    connect_scene_signals();
                    start_persistence_monitor();
                    log_message("HIDE_BANNER: FREE USER - Protection system fully re-enabled");
                } else {
                    log_message("HIDE_BANNER: FREE USER - Skipping auto-restore - ad frequency limit active");
                }
            }
            // Clear the flag after the hide period is over
            m_intentional_hide_in_progress.store(false);
            log_message("HIDE_BANNER: FREE USER - Signal protection disabled after hide period");
        }).detach();
        
        // Also clear the flag after a short delay in case the jthread doesn't work
        std::jthread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(10));  // Safety timeout
            m_intentional_hide_in_progress.store(false);
            
            // Safety re-enable of protection system
            if (!PremiumStatusHandler::is_premium(this)) {
                log_message("HIDE_BANNER: FREE USER - Safety re-enabling protection system");
                connect_scene_signals();
                start_persistence_monitor();
            }
            log_message("HIDE_BANNER: FREE USER - Signal protection safety timeout reached");
        }).detach();
        
        log_message("HIDE_BANNER: FREE USER - Banner temporarily hidden (5sec auto-restore - upgrade for full control)");
        return;
    }
    
    if (m_banner_persistent) {
        log_message("HIDE_BANNER: Cannot hide banner - it's in persistent UNHIDEABLE mode!");
        return;
    }
    
    // Premium users can hide freely
    PremiumStatusHandler::log_premium_action(this, "banner hiding", "full control");
    remove_banner_from_scenes();
    log_message("HIDE_BANNER: PREMIUM USER - Banner hidden (full control)");
}

void banner_manager::toggle_banner()
{
    log_message("TOGGLE_BANNER: Starting toggle operation...");
    
    // Get REAL visibility status from scenes (not just the flag)
    bool actually_visible = is_banner_visible();
    log_message("TOGGLE_BANNER: Current visibility - Flag: " + std::string(m_banner_visible ? "true" : "false") + 
                ", Actual: " + std::string(actually_visible ? "true" : "false"));
    
    // Use actual visibility instead of just the flag
    if (actually_visible) {
        log_message("TOGGLE_BANNER: Banner is visible, attempting to hide...");
        hide_banner();
    } else {
        log_message("TOGGLE_BANNER: Banner is hidden, attempting to show...");
        show_banner();
    }
    
    log_message("TOGGLE_BANNER: Toggle operation completed");
}

void banner_manager::set_banner_content(std::string_view content_data, std::string_view content_type)
{
    m_current_banner_content = content_data;
    m_current_content_type = content_type;
    
    // CRITICAL SECURITY FIX: Preserve visibility state during content update to prevent security bypass
    bool was_visible_before_update = m_banner_visible;
    log_message("BANNER CONTENT: SECURITY - Preserving visibility state during update - Was visible: " + std::string(was_visible_before_update ? "true" : "false"));
    
    // If banner source exists, release it and create new one
    if (m_banner_source) {
        obs_source_release(m_banner_source);
        m_banner_source = nullptr;
    }
    
    create_banner_source(content_data, content_type);
    
    // If banner was visible, update it in the scene
    if (was_visible_before_update) {
        log_message("BANNER CONTENT: Banner was visible before update - refreshing content in scenes");
        
        // Temporarily remove old content (this will set m_banner_visible = false - SECURITY RISK!)
        remove_banner_from_scenes();
        
        // CRITICAL SECURITY FIX: Immediately restore the visibility flag to prevent security bypass
        m_banner_visible = was_visible_before_update;
        log_message("BANNER CONTENT: SECURITY FIX - Restored visibility flag after remove_banner_from_scenes()");
        
        if (!PremiumStatusHandler::is_premium(this)) {
            // FREE USERS: Content change triggers ALL scenes update
            log_message("FREE USER: Content changed - updating banners across ALL scenes");
            initialize_banners_all_scenes();
            
            // EXTRA SECURITY: Re-enable protection monitoring for free users
            if (!m_persistence_monitor_active) {
                log_message("FREE USER: SECURITY - Re-enabling protection monitoring after content update");
                start_persistence_monitor();
            }
        } else {
            // PREMIUM USERS: Content change only affects current scene (if they choose)
            PremiumStatusHandler::log_premium_action(this, "content change", "use show_premium_banner() to add to current scene");
        }
        
        // Final security verification
        m_banner_visible = was_visible_before_update;
        log_message("BANNER CONTENT: SECURITY - Final visibility verification: " + std::string(m_banner_visible ? "true" : "false"));
    } else {
        log_message("BANNER CONTENT: Banner was not visible before update - no scene refresh needed");
    }
    
    log_message(std::format("Banner content set - Type: {} (User: {})", content_type, PremiumStatusHandler::get_user_type_string(this)));
}

void banner_manager::set_banner_content_with_custom_params(std::string_view content_data, std::string_view content_type,
                                                          std::string_view custom_css, int custom_width, int custom_height, bool css_locked)
{
    m_current_banner_content = content_data;
    m_current_content_type = content_type;
    
    log_message(std::format("ENHANCED BANNER: Setting content with custom parameters - Type: {}, CSS: {}, Size: {}x{}", 
                          content_type, (custom_css.empty() ? "default" : "custom"), custom_width, custom_height));
    
    // CRITICAL SECURITY FIX: Preserve visibility state during content update to prevent security bypass
    bool was_visible_before_update = m_banner_visible;
    log_message("ENHANCED BANNER: SECURITY - Preserving visibility state during update - Was visible: " + std::string(was_visible_before_update ? "true" : "false"));
    
    // If banner source exists, release it and create new one
    if (m_banner_source) {
        obs_source_release(m_banner_source);
        m_banner_source = nullptr;
    }
    
    create_banner_source_with_custom_params(content_data, content_type, custom_css, custom_width, custom_height, css_locked);
    
    // If banner was visible, update it in the scene WITHOUT removing it
    if (was_visible_before_update) {
        log_message("ENHANCED BANNER: Banner was visible before update - updating content WITHOUT removing from scenes");
        
        // DO NOT remove banner from scenes - just refresh the content in place
        // The create_banner_source_with_custom_params already updated the browser source content
        
        if (!PremiumStatusHandler::is_premium(this)) {
            // FREE USERS: Content updated in-place, just refresh protection
            log_message("FREE USER: Enhanced content changed - banners updated in-place (full-screen)");
            
            // EXTRA SECURITY: Force lock all banners after content update
            obs_frontend_source_list scenes = {};
            obs_frontend_get_scenes(&scenes);
            
            for (size_t i = 0; i < scenes.sources.num; i++) {
                obs_source_t* scene_source = scenes.sources.array[i];
                if (!scene_source) continue;
                
                obs_scene_t* scene = obs_scene_from_source(scene_source);
                if (!scene) continue;
                
                obs_sceneitem_t* banner_item = find_vortideck_banner_in_scene(scene);
                if (banner_item) {
                    lock_banner_item(banner_item);
                }
            }
            obs_frontend_source_list_free(&scenes);
            
            log_message("FREE USER: SECURITY - All banners re-locked after content update");
            
            // CRITICAL: Re-enable protection monitoring for free users
            if (!m_persistence_monitor_active) {
                log_message("FREE USER: SECURITY - Re-enabling protection monitoring after content update");
                start_persistence_monitor();
            }
        } else {
            // PREMIUM USERS: Content change only affects current scene (if they choose)
            PremiumStatusHandler::log_premium_action(this, "enhanced content change", "use show_premium_banner() to add to current scene");
        }
        
        // Final security verification
        m_banner_visible = was_visible_before_update;
        log_message("ENHANCED BANNER: SECURITY - Final visibility verification: " + std::string(m_banner_visible ? "true" : "false"));
    } else {
        log_message("ENHANCED BANNER: Banner was not visible before update - no scene refresh needed");
    }
    
    // CRITICAL: Ensure protection systems remain active for free users after ANY content update
    if (!PremiumStatusHandler::is_premium(this)) {
        log_message("FREE USER: SECURITY - Ensuring protection system remains active after content update");
        
        // Force re-enable signal connections if needed
        if (!m_signals_connected.load()) {
            log_message("FREE USER: WARNING - Signal connections lost, re-enabling");
            enable_signal_connections_when_safe();
        }
        
        // Force re-enable persistence monitoring if needed  
        if (!m_persistence_monitor_active) {
            log_message("FREE USER: WARNING - Persistence monitor inactive, re-enabling");
            start_persistence_monitor();
        }
        
        log_message("FREE USER: SECURITY - Protection systems verified active after content update");
    }
    
    log_message(std::format("Enhanced banner content set - Type: {} (User: {})", content_type, PremiumStatusHandler::get_user_type_string(this)));
}



void banner_manager::show_premium_banner()
{
    // PREMIUM USERS ONLY: Add banner to current scene by choice
    if (!PremiumStatusHandler::handle_premium_restriction(this, "banner_position", "premium banner function")) {
        log_message("FREE USER: Cannot use premium banner function - banners are automatically managed");
        return;
    }
    
    if (!m_banner_source) {
        log_message("PREMIUM USER: No banner content set - use WebSocket API to set content first");
        return;
    }
    
    // Add to current scene only (premium user's choice)
    add_banner_to_current_scene();
    m_banner_visible = true;
    
    PremiumStatusHandler::log_premium_action(this, "banner added to current scene", "you have full control");
}

bool banner_manager::is_banner_visible() const
{
    if (!m_banner_source) {
        return false;
    }
    
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    bool found_visible = false;
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* source = scenes.sources.array[i];
        if (!source) continue;
        
        obs_scene_t* scene = obs_scene_from_source(source);
        if (!scene) continue;
        
        // Use metadata-based detection instead of name-based
        obs_sceneitem_t* item = find_vortideck_banner_in_scene(scene);
        if (item && obs_sceneitem_visible(item)) {
            found_visible = true;
            break;
        }
    }
    
    obs_frontend_source_list_free(&scenes);
    
    // Update the flag to match reality
    const_cast<banner_manager*>(this)->m_banner_visible = found_visible;
    
    return found_visible;
}

std::string banner_manager::get_current_banner_content() const
{
    return m_current_banner_content;
}

obs_source_t* banner_manager::get_banner_source() const
{
    return m_banner_source;
}

void banner_manager::add_banner_menu()
{
    // Add VortiDeck banner menu to OBS
    obs_frontend_add_tools_menu_item("VortiDeck Banner", banner_menu_callback, this);
    log_message("VortiDeck Banner menu added to OBS Tools menu");
}

void banner_manager::banner_menu_callback(void* data)
{
    // Menu callback - behavior depends on user type
    if (data) {
        banner_manager* manager = static_cast<banner_manager*>(data);
        
        // Check user type first
        bool is_premium = manager->m_is_premium.load();
        
        if (is_premium) {
            // PREMIUM USERS: Menu does nothing - they have complete freedom
            manager->log_message("PREMIUM USER: Menu action ignored - you have complete banner freedom");
            manager->log_message("PREMIUM USER: Use WebSocket API to add banners if desired");
            return;
        }
        
        // FREE USERS ONLY: Show demo banner
        manager->log_message("FREE USER: Menu action - checking banner initialization");
        
        // PREVENT MULTIPLE CALLS - Check if banners exist across ALL scenes (not just current)
        if (manager->m_banner_source && manager->is_banner_visible()) {
            manager->log_message("FREE USER: Banners already initialized - menu action ignored to prevent duplicates");
            return;
        }
        
        // Check if content is set - if not, create a transparent placeholder
        if (manager->get_current_banner_content().empty()) {
            manager->log_message("FREE USER: Creating transparent placeholder banner (menu action)");
            // Create a transparent placeholder instead of red
            manager->set_banner_content("transparent", "transparent");  // Transparent banner as default
        }
        
        // Show the banner - FREE USERS get FORCED banners in ALL scenes
        manager->show_banner();
        
        manager->log_message("FREE USER: Demo banner created via menu - upgrade to premium for banner freedom");
    }
}

void banner_manager::create_banner_source(std::string_view content_data, std::string_view content_type)
{
    log_message(std::format("BANNER CREATION: Starting banner source creation - Type: {}, Data: {}...", content_type, content_data.substr(0, 50)));
    
    // IMPROVEMENT: Check if a banner source already exists with the base name
         // Convert content to URL format for browser source (needed for both reuse and create)
     std::string url_content;
     std::string css_content = "";
     
     if (content_type == "transparent") {
         // Create completely transparent banner (no visible content)
         url_content = "data:text/html,<html><body></body></html>";
         css_content = "body { margin: 0; padding: 0; background: transparent; width: 100vw; height: 100vh; overflow: hidden; }";
     } else if (content_type == "color") {
         // Create HTML page with color background
         std::string color = "#FF0000"; // Default red
         if (content_data.length() >= 7 && content_data[0] == '#') {
             color = content_data;
         }
         url_content = "data:text/html,<html><body><h2>VortiDeck Banner</h2></body></html>";
         css_content = std::format("body {{ background-color: {}; margin: 0; padding: 20px; font-family: Arial; color: white; text-align: center; display: flex; align-items: center; justify-content: center; }}", color);
     } else if (content_type == "text") {
         // Create HTML page with text content
         url_content = std::format("data:text/html,<html><body>{}</body></html>", content_data);
         css_content = "body { background-color: #000; margin: 0; padding: 20px; font-family: Arial, sans-serif; color: white; text-align: center; font-size: 24px; display: flex; align-items: center; justify-content: center; }";
     } else if (is_image_content(content_type) || is_video_content(content_type)) {
         // For image/video URLs - create HTML wrapper for proper CSS targeting
         if (is_url(content_data)) {
             // Create HTML wrapper with image/video element
             std::string element_tag = is_video_content(content_type) ? "video" : "img";
             std::string extra_attrs = is_video_content(content_type) ? " autoplay loop muted" : "";
             std::string default_css = std::format("body {{ margin: 0; padding: 0; background: transparent; display: flex; align-items: center; justify-content: center; width: 100vw; height: 100vh; overflow: hidden; }} {} {{ width: auto; height: auto; max-width: 100%; max-height: 100%; object-fit: contain; border: none; }}", element_tag);
             
             url_content = std::format("data:text/html,<html><head><style>{}</style></head><body><{} src=\"{}\"{}></{}</body></html>", 
                                     default_css, element_tag, content_data, extra_attrs, element_tag);
             css_content = ""; // CSS is now embedded in HTML
         } else {
             // Local file or other image/video path - same wrapper approach
             std::string element_tag = is_video_content(content_type) ? "video" : "img";
             std::string extra_attrs = is_video_content(content_type) ? " autoplay loop muted" : "";
             std::string default_css = std::format("body {{ margin: 0; padding: 0; background: transparent; display: flex; align-items: center; justify-content: center; width: 100vw; height: 100vh; overflow: hidden; }} {} {{ width: auto; height: auto; max-width: 100%; max-height: 100%; object-fit: contain; border: none; }}", element_tag);
             
             url_content = std::format("data:text/html,<html><head><style>{}</style></head><body><{} src=\"{}\"{}></{}</body></html>", 
                                     default_css, element_tag, content_data, extra_attrs, element_tag);
             css_content = ""; // CSS is now embedded in HTML
         }
     } else if (is_url(content_data)) {
         // Direct URL - use as is
         url_content = content_data;
         css_content = "";
     } else {
         // Default fallback - treat as text
         url_content = std::format("data:text/html,<html><body>{}</body></html>", content_data);
         css_content = "body { background-color: #000; margin: 0; padding: 20px; font-family: Arial, sans-serif; color: white; text-align: center; font-size: 24px; display: flex; align-items: center; justify-content: center; }";
     }

    obs_source_t* existing_source = obs_get_source_by_name(m_banner_source_name.c_str());
    if (existing_source) {
        // Check if existing source is actually a browser source
        const char* source_id = obs_source_get_id(existing_source);
        if (source_id && std::string(source_id) == "browser_source") {
            log_message("BANNER CREATION: Found existing browser source, reusing it");
        
        // Release our current source if any
        if (m_banner_source) {
            obs_source_release(m_banner_source);
        }
        
        // Use the existing source
        m_banner_source = existing_source;
        
            // Update the existing browser source with new URL content (use the shared conversion logic)
        obs_data_t* settings = obs_data_create();
            
                         // Set browser source properties with full-screen dimensions
             obs_data_set_string(settings, "url", url_content.c_str());
             
             // FULL-SCREEN BANNERS: Always use full canvas size
             obs_video_info ovi;
             obs_get_video_info(&ovi);
             
             int canvas_width = ovi.base_width > 0 ? ovi.base_width : 1920;
             int canvas_height = ovi.base_height > 0 ? ovi.base_height : 1080;
             
             obs_data_set_int(settings, "width", canvas_width);
             obs_data_set_int(settings, "height", canvas_height);
             
             obs_data_set_int(settings, "fps", 30);
             obs_data_set_bool(settings, "reroute_audio", false);
             obs_data_set_bool(settings, "restart_when_active", true); // Force refresh on content change
             obs_data_set_bool(settings, "shutdown", false);
             obs_data_set_string(settings, "css", css_content.c_str());
             
             // Update the browser source
        obs_source_update(m_banner_source, settings);
             
             // Force a refresh by toggling the source active state
             obs_source_set_enabled(m_banner_source, false);
             obs_source_set_enabled(m_banner_source, true);
             
        obs_data_release(settings);
        
             log_message(std::format("BANNER CREATION: Successfully reused existing browser source - Type: {}", content_type));
             log_message(std::format("BANNER CREATION: Updated URL: {}{}", url_content.substr(0, 100), (url_content.length() > 100 ? "..." : "")));
             log_message("BANNER CREATION: Forced browser source refresh for new content");
        return;
        } else {
            // Existing source is NOT a browser source (probably old custom source) - remove it and create new browser source
            log_message("BANNER CREATION: Found existing NON-browser source (probably old custom), removing it");
            obs_source_release(existing_source);
            
            // Remove the old source from all scenes first
            obs_frontend_source_list scenes = {};
            obs_frontend_get_scenes(&scenes);
            
            for (size_t i = 0; i < scenes.sources.num; i++) {
                obs_source_t* scene_source = scenes.sources.array[i];
                if (!scene_source) continue;
                
                obs_scene_t* scene = obs_scene_from_source(scene_source);
                if (!scene) continue;
                
                obs_sceneitem_t* item = obs_scene_find_source(scene, m_banner_source_name.c_str());
                if (item) {
                    obs_sceneitem_remove(item);
                    log_message("BANNER CREATION: Removed old source from scene");
                }
            }
            obs_frontend_source_list_free(&scenes);
        }
    }
    
    // Create settings for browser_source directly
    obs_data_t* settings = obs_data_create();
    
         // Set browser source properties with full-screen dimensions
     obs_data_set_string(settings, "url", url_content.c_str());
     
     // FULL-SCREEN BANNERS: Always use full canvas size
     obs_video_info ovi;
     obs_get_video_info(&ovi);
     
     int canvas_width = ovi.base_width > 0 ? ovi.base_width : 1920;
     int canvas_height = ovi.base_height > 0 ? ovi.base_height : 1080;
     
     obs_data_set_int(settings, "width", canvas_width);
     obs_data_set_int(settings, "height", canvas_height);
     
     obs_data_set_int(settings, "fps", 30);
     obs_data_set_bool(settings, "reroute_audio", false);
     obs_data_set_bool(settings, "restart_when_active", true); // Help with content loading
     obs_data_set_bool(settings, "shutdown", false);
     obs_data_set_string(settings, "css", css_content.c_str());
     
     log_message("BANNER CREATION: Attempting to create browser_source directly...");
     log_message(std::format("BANNER CREATION: New source URL: {}{}", url_content.substr(0, 100), (url_content.length() > 100 ? "..." : "")));
     log_message(std::format("BANNER CREATION: Dimensions: {}x{} (Full Canvas)", canvas_width, canvas_height));
     
     // Create browser_source directly - simple and reliable
     m_banner_source = obs_source_create("browser_source", m_banner_source_name.c_str(), settings, nullptr);
    
    obs_data_release(settings);
    
    if (m_banner_source) {
        // Add VortiDeck protection metadata to the browser source
        obs_data_t* private_settings = obs_source_get_private_settings(m_banner_source);
        obs_data_set_string(private_settings, "vortideck_banner", "true");
        obs_data_set_string(private_settings, "vortideck_banner_id", "banner_v1");
        obs_data_set_string(private_settings, "vortideck_banner_type", "browser");
        obs_data_release(private_settings);
        
        log_message(std::format("BANNER CREATION: Successfully created browser_source with VortiDeck protection - Type: {}", content_type));
        
        // Add additional verification
        const char* source_type = obs_source_get_id(m_banner_source);
        uint32_t width = obs_source_get_width(m_banner_source);
        uint32_t height = obs_source_get_height(m_banner_source);
        
        log_message(std::format("BANNER CREATION: Source verification - Type: {}, Size: {}x{}", 
                               (source_type ? source_type : "NULL"), width, height));
    } else {
        log_message("BANNER CREATION: FAILED to create Browser Source for VortiDeck Banner!");
        log_message("BANNER CREATION: Possible issues:");
        log_message("BANNER CREATION: 1. Browser source plugin not available");
        log_message("BANNER CREATION: 2. OBS not fully initialized");
        log_message("BANNER CREATION: 3. Invalid source settings");
        
        // Fallback: Try to create a simple color source instead
        log_message("BANNER CREATION: Attempting fallback to color source...");
        
        obs_data_t* color_settings = obs_data_create();
        
        if (content_type == "color" && content_data.length() >= 7 && content_data[0] == '#') {
            // Parse hex color
            try {
                std::string hex = std::string(content_data.substr(1));
                uint32_t color_value = std::stoul(hex, nullptr, 16);
                // Convert RGB to RGBA (add alpha channel)
                color_value |= 0xFF000000;  // Full opacity
                obs_data_set_int(color_settings, "color", color_value);
            } catch (const std::exception&) {
                // Default to transparent if parsing fails
                obs_data_set_int(color_settings, "color", 0x00000000);  // Fully transparent
            }
        } else if (content_type == "transparent") {
            // Transparent fallback for transparent content type
            obs_data_set_int(color_settings, "color", 0x00000000);  // Fully transparent
        } else {
            // Default to transparent for any content type in fallback mode
            obs_data_set_int(color_settings, "color", 0x00000000);  // Fully transparent
        }
        
        obs_data_set_int(color_settings, "width", 1920);
        obs_data_set_int(color_settings, "height", 100);
        
        // Try to create a color source as fallback
        m_banner_source = obs_source_create("color_source", m_banner_source_name.c_str(), color_settings, nullptr);
        
        obs_data_release(color_settings);
        
        if (m_banner_source) {
            // Add VortiDeck protection metadata even to fallback source
            obs_data_t* private_settings = obs_source_get_private_settings(m_banner_source);
            obs_data_set_string(private_settings, "vortideck_banner", "true");
            obs_data_set_string(private_settings, "vortideck_banner_id", "banner_v1");
            obs_data_set_string(private_settings, "vortideck_banner_type", "color_fallback");
            obs_data_release(private_settings);
            
            log_message("BANNER CREATION: FALLBACK SUCCESS - Created color source with VortiDeck protection");
        } else {
            log_message("BANNER CREATION: COMPLETE FAILURE - Could not create any source type");
        }
    }
}

void banner_manager::create_banner_source_with_custom_params(std::string_view content_data, std::string_view content_type,
                                                             std::string_view custom_css, int custom_width, int custom_height, bool css_locked)
{
    log_message(std::format("ENHANCED BANNER CREATION: Starting banner source creation with custom params - Type: {}, Data: {}...", 
                          content_type, content_data.substr(0, 50)));
    log_message(std::format("ENHANCED BANNER CREATION: Custom CSS: {}, Dimensions: {}x{}", 
                          (custom_css.empty() ? "none" : "provided"), custom_width, custom_height));
    
    // Convert content to URL format for browser source (needed for both reuse and create)
    std::string url_content;
    std::string css_content = "";
    
    if (content_type == "transparent") {
        // Create completely transparent banner (no visible content)
        url_content = "data:text/html,<html><body></body></html>";
        css_content = custom_css.empty() ? 
            "body { margin: 0; padding: 0; background: transparent; width: 100vw; height: 100vh; overflow: hidden; }" :
            std::string(custom_css);
    } else if (content_type == "color") {
        // Create HTML page with color background
        std::string color = "#FF0000"; // Default red
        if (content_data.length() >= 7 && content_data[0] == '#') {
            color = std::string(content_data);
        }
        url_content = "data:text/html,<html><body><h2>VortiDeck Banner</h2></body></html>";
        css_content = custom_css.empty() ? 
            std::format("body {{ background-color: {}; margin: 0; padding: 20px; font-family: Arial; color: white; text-align: center; display: flex; align-items: center; justify-content: center; }}", color) :
            std::string(custom_css);
    } else if (content_type == "text") {
        // Create HTML page with text content
        url_content = std::format("data:text/html,<html><body>{}</body></html>", content_data);
        css_content = custom_css.empty() ? 
            "body { background-color: #000; margin: 0; padding: 20px; font-family: Arial, sans-serif; color: white; text-align: center; font-size: 24px; display: flex; align-items: center; justify-content: center; }" :
            std::string(custom_css);
                    } else if (is_image_content(content_type) || is_video_content(content_type)) {
        // For image/video URLs - check if custom CSS needs HTML structure
        if (is_url(content_data)) {
            // Check if custom CSS references HTML elements (img, video, etc.)
            bool needs_html_wrapper = !custom_css.empty() && 
                                    (custom_css.find("img") != std::string::npos || 
                                     custom_css.find("video") != std::string::npos ||
                                     custom_css.find("@keyframes") != std::string::npos ||
                                     custom_css.find("animation") != std::string::npos);
            
            if (needs_html_wrapper) {
                // Create HTML wrapper with image/video element for CSS targeting
                std::string element_tag = is_video_content(content_type) ? "video" : "img";
                std::string extra_attrs = is_video_content(content_type) ? " autoplay loop muted" : "";
                
                // FLEXIBLE CSS HANDLING: Use explicit css_locked parameter with smart defaults
                // If css_locked is not explicitly set, use smart defaults:
                // - Premium users: CSS editable by default 
                // - Free users: CSS locked by default (for ad content protection)
                bool final_css_locked = css_locked;
                if (!css_locked && !PremiumStatusHandler::is_premium(this)) {
                    // Free users get locked CSS by default unless explicitly overridden
                    final_css_locked = true;
                    log_message("BANNER CREATION: Auto-locking CSS for free user (set css_locked=false to override)");
                }
                
                if (final_css_locked) {
                    // LOCKED MODE: Embed CSS in HTML (ads, protected content)
                    url_content = std::format("data:text/html,<html><head><style>{}</style></head><body><{} src=\"{}\"{}></{}</body></html>", 
                                            custom_css, element_tag, content_data, extra_attrs, element_tag);
                    css_content = ""; // CSS is hidden in HTML
                    log_message("BANNER CREATION: CSS LOCKED - Embedded in HTML (not visible in OBS properties)");
                } else {
                    // EDITABLE MODE: CSS visible in OBS browser source properties
                    // Create basic HTML structure without embedded CSS
                    url_content = std::format("data:text/html,<html><body><{} src=\"{}\"{}></{}</body></html>", 
                                            element_tag, content_data, extra_attrs, element_tag);
                    css_content = std::string(custom_css); // CSS will be visible in OBS
                    log_message("BANNER CREATION: CSS EDITABLE - Visible in OBS properties for inspection/modification");
                }
                
                log_message(std::format("BANNER CREATION: Created HTML wrapper for image/video with custom CSS - Mode: {}", 
                                       final_css_locked ? "LOCKED" : "EDITABLE"));
            } else {
                // Direct image/video URL (no custom CSS or CSS doesn't need HTML structure)
                url_content = std::string(content_data);
                css_content = custom_css.empty() ? 
                    "body { margin: 0; padding: 0; background: transparent; overflow: hidden; }" :
                    std::string(custom_css);
            }
        } else {
            // Local file or other image URL - same logic
            bool needs_html_wrapper = !custom_css.empty() && 
                                    (custom_css.find("img") != std::string::npos || 
                                     custom_css.find("video") != std::string::npos ||
                                     custom_css.find("@keyframes") != std::string::npos ||
                                     custom_css.find("animation") != std::string::npos);
            
            if (needs_html_wrapper) {
                std::string element_tag = is_video_content(content_type) ? "video" : "img";
                std::string extra_attrs = is_video_content(content_type) ? " autoplay loop muted" : "";
                
                // FLEXIBLE CSS HANDLING: Use explicit css_locked parameter with smart defaults
                // If css_locked is not explicitly set, use smart defaults:
                // - Premium users: CSS editable by default 
                // - Free users: CSS locked by default (for ad content protection)
                bool final_css_locked = css_locked;
                if (!css_locked && !PremiumStatusHandler::is_premium(this)) {
                    // Free users get locked CSS by default unless explicitly overridden
                    final_css_locked = true;
                    log_message("BANNER CREATION: Auto-locking CSS for free user (set css_locked=false to override)");
                }
                
                if (final_css_locked) {
                    // LOCKED MODE: Embed CSS in HTML (ads, protected content)
                    url_content = std::format("data:text/html,<html><head><style>{}</style></head><body><{} src=\"{}\"{}></{}>></body></html>", 
                                            custom_css, element_tag, content_data, extra_attrs, element_tag);
                    css_content = ""; // CSS is hidden in HTML
                    log_message("BANNER CREATION: CSS LOCKED - Embedded in HTML (not visible in OBS properties)");
                } else {
                    // EDITABLE MODE: CSS visible in OBS browser source properties
                    // Create basic HTML structure without embedded CSS
                    url_content = std::format("data:text/html,<html><body><{} src=\"{}\"{}></{}>></body></html>", 
                                            element_tag, content_data, extra_attrs, element_tag);
                    css_content = std::string(custom_css); // CSS will be visible in OBS
                    log_message("BANNER CREATION: CSS EDITABLE - Visible in OBS properties for inspection/modification");
                }
                
                log_message(std::format("BANNER CREATION: Local file HTML wrapper with custom CSS - Mode: {}", 
                           final_css_locked ? "LOCKED" : "EDITABLE"));
            } else {
                url_content = std::string(content_data);
                css_content = custom_css.empty() ? 
                    "body { margin: 0; padding: 0; background: transparent; overflow: hidden; }" :
                    std::string(custom_css);
            }
        }
    } else if (is_url(content_data)) {
        // Direct URL - use as is
        url_content = std::string(content_data);
        css_content = std::string(custom_css); // Use provided CSS or empty
    } else {
        // Default fallback - treat as text
        url_content = std::format("data:text/html,<html><body>{}</body></html>", content_data);
        css_content = custom_css.empty() ? 
            "body { background-color: #000; margin: 0; padding: 20px; font-family: Arial, sans-serif; color: white; text-align: center; font-size: 24px; display: flex; align-items: center; justify-content: center; }" :
            std::string(custom_css);
    }

    obs_source_t* existing_source = obs_get_source_by_name(m_banner_source_name.c_str());
    if (existing_source) {
        // Check if existing source is actually a browser source
        const char* source_id = obs_source_get_id(existing_source);
        if (source_id && std::string(source_id) == "browser_source") {
            log_message("ENHANCED BANNER CREATION: Found existing browser source, reusing it");
            
            // Release our current source if any
            if (m_banner_source) {
                obs_source_release(m_banner_source);
            }
            
            // Use the existing source
            m_banner_source = existing_source;
            
            // Update the existing browser source with new URL content (use the shared conversion logic)
            obs_data_t* settings = obs_data_create();
            
            // Set browser source properties with custom dimensions
            obs_data_set_string(settings, "url", url_content.c_str());
            
            // FULL-SCREEN BANNERS: Use custom dimensions if provided, otherwise use full canvas size
            // Get OBS canvas resolution for full-screen banners
            obs_video_info ovi;
            obs_get_video_info(&ovi);
            
            int canvas_width = ovi.base_width > 0 ? ovi.base_width : 1920;
            int canvas_height = ovi.base_height > 0 ? ovi.base_height : 1080;
            
            // ALWAYS use full canvas size for browser source (ignore custom width/height)
            int width = canvas_width;
            int height = canvas_height;
            
            log_message("FULL-SCREEN BANNER: Browser source dimensions " + std::to_string(width) + "x" + std::to_string(height) + 
                       " (Full Canvas - Custom w/h " + std::to_string(custom_width) + "x" + std::to_string(custom_height) + " for CSS only)");
            
            obs_data_set_int(settings, "width", width);
            obs_data_set_int(settings, "height", height);
            
            obs_data_set_int(settings, "fps", 30);
            obs_data_set_bool(settings, "reroute_audio", false);
            obs_data_set_bool(settings, "restart_when_active", true); // Force refresh on content change
            obs_data_set_bool(settings, "shutdown", false);
            obs_data_set_string(settings, "css", css_content.c_str());
            
            // Update the browser source
            obs_source_update(m_banner_source, settings);
            
            // Force a refresh by toggling the source active state AND triggering browser refresh
            obs_source_set_enabled(m_banner_source, false);
            obs_source_set_enabled(m_banner_source, true);
            
            // Force browser source refresh for new content
            log_message("ENHANCED BANNER CREATION: Forced browser source refresh for new content");
            
            obs_data_release(settings);
            
            log_message(std::format("ENHANCED BANNER CREATION: Successfully reused existing browser source - Type: {}", content_type));
            log_message(std::format("ENHANCED BANNER CREATION: Updated URL: {}{}", url_content.substr(0, 100), (url_content.length() > 100 ? "..." : "")));
            log_message(std::format("ENHANCED BANNER CREATION: Dimensions: {}x{}", width, height));
            log_message(std::format("ENHANCED BANNER CREATION: CSS: {}", (custom_css.empty() ? "default" : "custom")));
            return;
        } else {
            // Existing source is NOT a browser source (probably old custom source) - remove it and create new browser source
            log_message("ENHANCED BANNER CREATION: Found existing NON-browser source (probably old custom), removing it");
            obs_source_release(existing_source);
            
            // Remove the old source from all scenes first
            obs_frontend_source_list scenes = {};
            obs_frontend_get_scenes(&scenes);
            
            for (size_t i = 0; i < scenes.sources.num; i++) {
                obs_source_t* scene_source = scenes.sources.array[i];
                if (!scene_source) continue;
                
                obs_scene_t* scene = obs_scene_from_source(scene_source);
                if (!scene) continue;
                
                obs_sceneitem_t* item = obs_scene_find_source(scene, m_banner_source_name.c_str());
                if (item) {
                    obs_sceneitem_remove(item);
                    log_message("ENHANCED BANNER CREATION: Removed old source from scene");
                }
            }
            obs_frontend_source_list_free(&scenes);
        }
    }
    
    // Create settings for browser_source directly
    obs_data_t* settings = obs_data_create();
    
    // Set browser source properties with custom dimensions
    obs_data_set_string(settings, "url", url_content.c_str());
    
    // FULL-SCREEN BANNERS: Use custom dimensions if provided, otherwise use full canvas size
    // Get OBS canvas resolution for full-screen banners
    obs_video_info ovi;
    obs_get_video_info(&ovi);
    
    int canvas_width = ovi.base_width > 0 ? ovi.base_width : 1920;
    int canvas_height = ovi.base_height > 0 ? ovi.base_height : 1080;
    
    // ALWAYS use full canvas size for browser source (ignore custom width/height)
    int width = canvas_width;
    int height = canvas_height;
    
    log_message(std::format("FULL-SCREEN BANNER: Browser source dimensions {}x{} (Full Canvas - Custom w/h {}x{} for CSS only)", 
                           width, height, custom_width, custom_height));
    
    obs_data_set_int(settings, "width", width);
    obs_data_set_int(settings, "height", height);
    obs_data_set_int(settings, "fps", 30);
    obs_data_set_bool(settings, "reroute_audio", false);
    obs_data_set_bool(settings, "restart_when_active", true); // Help with content loading
    obs_data_set_bool(settings, "shutdown", false);
    obs_data_set_string(settings, "css", css_content.c_str());
    
    log_message("ENHANCED BANNER CREATION: Attempting to create browser_source directly...");
    log_message(std::format("ENHANCED BANNER CREATION: New source URL: {}{}", url_content.substr(0, 100), (url_content.length() > 100 ? "..." : "")));
    log_message(std::format("ENHANCED BANNER CREATION: Dimensions: {}x{}", width, height));
    log_message(std::format("ENHANCED BANNER CREATION: CSS: {}", (custom_css.empty() ? "default" : "custom")));
    
    // Create browser_source directly - simple and reliable
    m_banner_source = obs_source_create("browser_source", m_banner_source_name.c_str(), settings, nullptr);
    
    obs_data_release(settings);
    
    if (m_banner_source) {
        // Add VortiDeck protection metadata to the browser source
        obs_data_t* private_settings = obs_source_get_private_settings(m_banner_source);
        obs_data_set_string(private_settings, "vortideck_banner", "true");
        obs_data_set_string(private_settings, "vortideck_banner_id", "banner_v1");
        obs_data_set_string(private_settings, "vortideck_banner_type", "browser");
        obs_data_release(private_settings);
        
        log_message(std::format("ENHANCED BANNER CREATION: Successfully created browser_source with VortiDeck protection - Type: {}", content_type));
        
        // Add additional verification
        const char* source_type = obs_source_get_id(m_banner_source);
        uint32_t actual_width = obs_source_get_width(m_banner_source);
        uint32_t actual_height = obs_source_get_height(m_banner_source);
        
        log_message(std::format("ENHANCED BANNER CREATION: Source verification - Type: {}, Size: {}x{}", 
                               (source_type ? source_type : "NULL"), actual_width, actual_height));
    } else {
        log_message("ENHANCED BANNER CREATION: FAILED to create Browser Source for VortiDeck Banner!");
        
        // Fallback to regular banner creation
        log_message("ENHANCED BANNER CREATION: Falling back to regular banner creation...");
        create_banner_source(content_data, content_type);
    }
}

void banner_manager::add_banner_to_current_scene()
{
    if (!m_banner_source) {
        log_message("No banner source to add");
        return;
    }
    
    obs_source_t* current_scene_source = obs_frontend_get_current_scene();
    if (!current_scene_source) {
        log_message("No current scene");
        return;
    }
    
    obs_scene_t* scene = obs_scene_from_source(current_scene_source);
    if (!scene) {
        obs_source_release(current_scene_source);
        log_message("Could not get scene from source");
        return;
    }
    
    // CRITICAL: Check if banner already exists in scene to prevent duplicates (metadata-based)
    int banner_count = count_vortideck_banners_in_scene(scene);
    if (banner_count > 0) {
        // Banner(s) already exist - just ensure first one is visible and properly configured
        obs_sceneitem_t* existing_item = find_vortideck_banner_in_scene(scene);
    if (existing_item) {
        obs_sceneitem_set_visible(existing_item, true);
        lock_banner_item(existing_item);
        }
        log_message(std::format("Banner already exists in scene ({} found) - NO NEW CREATION", banner_count));
    } else {
        // Add new scene item ONLY if it doesn't exist
        obs_sceneitem_t* scene_item = obs_scene_add(scene, m_banner_source);
        if (scene_item) {
            // Position banner at 0,0 to fill entire screen (CSS controls content positioning)
            vec2 pos;
            pos.x = 0;
            pos.y = 0; // Top-left corner (0,0)
            obs_sceneitem_set_pos(scene_item, &pos);
            
            // Set size to fill entire screen
            vec2 scale;
            scale.x = 1.0f;
            scale.y = 1.0f;
            obs_sceneitem_set_scale(scene_item, &scale);
            
            lock_banner_item(scene_item);
            log_message("NEW banner added to current scene (1 per scene max)");
        } else {
            log_message("Failed to add banner to scene");
        }
    }
    
    obs_source_release(current_scene_source);
}

void banner_manager::initialize_banners_all_scenes()
{
    // This function is ONLY for FREE USERS - premium users have complete freedom
    if (PremiumStatusHandler::is_premium(this)) {
        PremiumStatusHandler::log_premium_action(this, "banner initialization", "SKIPPED - complete banner freedom");
        return;
    }
    
    if (!m_banner_source) {
        log_message("FREE USER: No banner source to initialize across scenes");
        return;
    }
    
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    int scenes_initialized = 0;
    int scenes_already_covered = 0;
    int total_scenes = scenes.sources.num;
    
    log_message(std::format("FREE USER: FORCED banner initialization - checking {} scenes", total_scenes));
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* scene_source = scenes.sources.array[i];
        if (!scene_source) continue;
        
        obs_scene_t* scene = obs_scene_from_source(scene_source);
        if (!scene) continue;
        
        // Get scene name for logging
        const char* scene_name = obs_source_get_name(scene_source);
        
        // Check if banner already exists in this scene (metadata-based detection)
        int banner_count = count_vortideck_banners_in_scene(scene);
        
        log_message("FREE USER: Scene '" + std::string(scene_name) + "' - Found " + std::to_string(banner_count) + " VortiDeck banners");
        
        if (banner_count > 0) {
            // Banner(s) already exist - just ensure first one is visible and properly configured
            obs_sceneitem_t* existing_item = find_vortideck_banner_in_scene(scene);
        if (existing_item) {
            obs_sceneitem_set_visible(existing_item, true);
            lock_banner_item(existing_item);
            }
            scenes_already_covered++;
            log_message("FREE USER: Scene '" + std::string(scene_name) + "' already has " + std::to_string(banner_count) + " banner(s) - NO NEW CREATION");
        } else {
            // Scene missing banner - initialize it
            obs_sceneitem_t* scene_item = obs_scene_add(scene, m_banner_source);
            if (scene_item) {
                // Position banner at 0,0 to fill entire screen (CSS controls content positioning)
                vec2 pos;
                pos.x = 0;
                pos.y = 0; // Top-left corner (0,0)
                obs_sceneitem_set_pos(scene_item, &pos);
                
                // Set size to fill entire screen
                vec2 scale;
                scale.x = 1.0f;
                scale.y = 1.0f;
                obs_sceneitem_set_scale(scene_item, &scale);
                
                // Make it visible and locked
                obs_sceneitem_set_visible(scene_item, true);
                lock_banner_item(scene_item);
                obs_sceneitem_set_order(scene_item, OBS_ORDER_MOVE_TOP);
                
                scenes_initialized++;
                log_message("FREE USER: Scene '" + std::string(scene_name) + "' missing banner - INITIALIZED");
            } else {
                log_message("ERROR: Failed to initialize banner in scene '" + std::string(scene_name) + "'");
            }
        }
    }
    
    obs_frontend_source_list_free(&scenes);
    
    log_message("FREE USER: Banner initialization complete - " + std::to_string(scenes_initialized) + " scenes initialized, " + 
                std::to_string(scenes_already_covered) + " scenes already covered (" + 
                std::to_string(total_scenes) + " total scenes)");
}

void banner_manager::remove_banner_from_scenes()
{
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    int scenes_hidden = 0;
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* source = scenes.sources.array[i];
        if (!source) continue;
        
        obs_scene_t* scene = obs_scene_from_source(source);
        if (!scene) continue;
        
        // Use metadata-based detection instead of name-based
        obs_sceneitem_t* item = find_vortideck_banner_in_scene(scene);
        if (item) {
            obs_sceneitem_set_visible(item, false);
            scenes_hidden++;
        }
    }
    
    obs_frontend_source_list_free(&scenes);
    
    // Update the visibility flag based on actual scene state
    if (scenes_hidden > 0) {
        m_banner_visible = false;
        log_message("Banner removed from " + std::to_string(scenes_hidden) + " scenes");
    } else {
        log_message("No banners found to remove from scenes");
    }
}

void banner_manager::lock_banner_item(obs_sceneitem_t* item)
{
    if (item) {
        bool is_premium = m_is_premium.load();
        
        // Always lock for positioning (free users can't move banners)
        obs_sceneitem_set_locked(item, true);
        
        if (!is_premium) {
            // Free users: Lock position and prevent manipulation
            log_message("FREE USER: Banner locked - no positioning control (upgrade to premium)");
            
            // Force banner to 0,0 position (full screen for CSS control)
            vec2 pos;
            pos.x = 0;  // Top-left corner
            pos.y = 0;  // Top-left corner (0,0 for full screen)
            obs_sceneitem_set_pos(item, &pos);
            
            // Lock scale to prevent resizing for free users
            vec2 scale;
            scale.x = 1.0f;
            scale.y = 1.0f;
            obs_sceneitem_set_scale(item, &scale);
            
            log_message("FREE USER: Banner forced to 0,0 full-screen position - CSS controls content");
        } else {
            // Premium users: Still locked but they have positioning API control
            log_message("PREMIUM USER: Banner locked but custom positioning enabled via API");
        }
    }
}

void banner_manager::make_banner_persistent()
{
    if (!m_banner_source) {
        log_message("No banner source to make persistent");
        return;
    }
    
    if (m_banner_persistent) {
        log_message("Banner already in persistent mode - ignoring duplicate request");
        return;
    }
    
    // Set persistent flag
    m_banner_persistent = true;
    
    // DON'T add to all scenes immediately - let the monitoring system handle it gradually
    // This prevents the massive banner creation burst that was happening
    
    log_message("Banner persistence mode ENABLED - will be maintained across scenes");
    
    // Start monitoring to enforce visibility (with 2-second intervals)
    start_persistence_monitor();
    
    // NO immediate enforcement check - this was causing the banner explosion
    // Let the monitoring jthread handle it gradually
    
    log_message("Banner made persistent - monitoring every 2 seconds (not aggressive creation)");
}

bool banner_manager::is_url(std::string_view content)
{
    return content.substr(0, 7) == "http://" || content.substr(0, 8) == "https://";
}



bool banner_manager::is_file_path(std::string_view content)
{
    // If it's not a URL, assume it's a file path
    return !is_url(content);
}

bool banner_manager::is_image_content(std::string_view content_type)
{
    std::string lower_type(content_type);
    std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(), ::tolower);
    
    return lower_type.find("image") != std::string::npos ||
           lower_type == "png" || lower_type == "jpg" || lower_type == "jpeg" ||
           lower_type == "gif" || lower_type == "bmp" || lower_type == "webp" ||
           lower_type == "tga";
}

bool banner_manager::is_video_content(std::string_view content_type)
{
    std::string lower_type(content_type);
    std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(), ::tolower);
    
    return lower_type.find("video") != std::string::npos ||
           lower_type == "mp4" || lower_type == "avi" || lower_type == "mov" ||
           lower_type == "mkv" || lower_type == "flv" || lower_type == "wmv" ||
           lower_type == "webm" || lower_type == "m4v";
}

// METADATA-BASED BANNER DETECTION (Better than name-based)
bool banner_manager::is_vortideck_banner_name(const char* name) const
{
    if (!name) return false;
    
    // Enhanced name checking - handles OBS auto-renaming patterns
    // OBS renames like: VortiDeck Banner -> VortiDeck Banner 2, VortiDeck Banner 3, etc.
    return strstr(name, "VortiDeck Banner") != nullptr;
}

bool banner_manager::is_vortideck_banner_by_metadata(obs_source_t* source) const
{
    if (!source) return false;
    
    obs_data_t* private_settings = obs_source_get_private_settings(source);
    if (!private_settings) {
        return false;
    }
    
    const char* vortideck_banner = obs_data_get_string(private_settings, "vortideck_banner");
    bool is_banner = vortideck_banner && std::string(vortideck_banner) == "true";
    
    obs_data_release(private_settings);
    
    // Only log when we find a banner, not on every check
    if (is_banner) {
        const char* source_name = obs_source_get_name(source);
        // Reduce logging frequency - only log occasionally to avoid spam
        static int log_counter = 0;
        if (++log_counter % 10 == 0) {  // Log every 10th detection
            const_cast<banner_manager*>(this)->log_message("METADATA: VortiDeck banner confirmed - " + std::string(source_name ? source_name : "unknown"));
        }
    }
    
    return is_banner;
}

bool banner_manager::is_vortideck_banner_item(obs_sceneitem_t* item) const
{
    if (!item) return false;
    
    obs_source_t* source = obs_sceneitem_get_source(item);
    if (!source) return false;
    
    // Method 1: Check metadata first (most reliable)
    if (is_vortideck_banner_by_metadata(source)) {
        return true;
    }
    
    // Method 2: Fallback to name-based detection (for existing banners without metadata)
    const char* source_name = obs_source_get_name(source);
    return is_vortideck_banner_name(source_name);
}

obs_sceneitem_t* banner_manager::find_vortideck_banner_in_scene(obs_scene_t* scene) const
{
    if (!scene) return nullptr;
    
    struct find_data {
        const banner_manager* manager;
        obs_sceneitem_t* found_item;
    };
    
    find_data data = { this, nullptr };
    
    // Enumerate all scene items to find our banner
    obs_scene_enum_items(scene, [](obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* data) {
        find_data* fd = static_cast<find_data*>(data);
        if (fd->manager->is_vortideck_banner_item(item)) {
            fd->found_item = item;
            return false; // Stop enumeration
        }
        return true; // Continue enumeration
    }, &data);
    
    return data.found_item;
}

int banner_manager::count_vortideck_banners_in_scene(obs_scene_t* scene) const
{
    if (!scene) return 0;
    
    struct count_data {
        const banner_manager* manager;
        int count;
    };
    
    count_data data = { this, 0 };
    
    // Enumerate all scene items to count our banners
    obs_scene_enum_items(scene, [](obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* data) {
        count_data* cd = static_cast<count_data*>(data);
        if (cd->manager->is_vortideck_banner_item(item)) {
            cd->count++;
        }
        return true; // Continue enumeration
    }, &data);
    
    return data.count;
}

void banner_manager::cleanup_banner_names_after_deletion()
{
    log_message("CLEANUP: Starting banner name cleanup after deletion");
    
    // Simple approach: Just log the current state and let OBS handle naming
    // The main issue is that we're creating multiple sources with the same name
    // which causes OBS to auto-number them. We should prevent this instead.
    
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    std::vector<std::string> banner_names;
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* scene_source = scenes.sources.array[i];
        if (!scene_source) continue;
        
        obs_scene_t* scene = obs_scene_from_source(scene_source);
        if (!scene) continue;
        
        // Count banners in this scene
        obs_scene_enum_items(scene, [](obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* data) -> bool {
            std::vector<std::string>* names = static_cast<std::vector<std::string>*>(data);
            
            obs_source_t* source = obs_sceneitem_get_source(item);
            if (!source) return true;
            
            const char* name = obs_source_get_name(source);
            if (!name) return true;
            
            // Check if this is a VortiDeck banner
            if (strstr(name, "VortiDeck Banner") != nullptr) {
                std::string name_str(name);
                // Check if not already in our list
                if (std::find(names->begin(), names->end(), name_str) == names->end()) {
                    names->push_back(name_str);
                }
            }
            
            return true;
        }, &banner_names);
    }
    
    obs_frontend_source_list_free(&scenes);
    
    log_message(std::format("CLEANUP: Found {} unique VortiDeck banners", banner_names.size()));
    for (const auto& name : banner_names) {
        log_message(std::format("CLEANUP: - {}", name));
    }
    
    // The real solution is to prevent multiple sources with the same name
    // This will be handled by improving the banner creation logic
    log_message("CLEANUP: Banner name cleanup completed (improved creation logic will prevent numbering)");
}

void banner_manager::rename_source_if_needed(obs_source_t* source, std::string_view target_name)
{
    if (!source) return;
    
    const char* current_name = obs_source_get_name(source);
    if (!current_name) return;
    
    // For now, just log the rename request
    // Source renaming in OBS is complex and can cause instability
    log_message(std::format("CLEANUP: Rename request - '{}' to '{}' (deferred)", current_name, target_name));
    
    // TODO: Implement safe source renaming if needed
    // For now, we'll focus on preventing the problem during creation
}


void banner_manager::log_message(std::string_view message) const
{
    // Use proper OBS logging directly
    blog(LOG_INFO, "[Banner Manager] %s", message.data());
}

// ============================================================================
// ANALYTICS TRACKING IMPLEMENTATION
// ============================================================================

void banner_manager::track_ad_display_start(const std::string& ad_id, const std::string& campaign_id, 
                                           int planned_duration_ms, const std::string& content_type)
{
    if (!m_analytics.tracking_enabled.load()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_analytics.analytics_mutex);
    
    // Store current display info
    m_current_ad_id = ad_id;
    m_current_campaign_id = campaign_id;
    m_current_display_start = std::chrono::system_clock::now();
    m_current_planned_duration.store(planned_duration_ms);
    
    // Create display metrics entry
    AdDisplayMetrics metrics;
    metrics.ad_id = ad_id;
    metrics.campaign_id = campaign_id;
    metrics.start_time = m_current_display_start;
    metrics.planned_duration_ms = planned_duration_ms;
    metrics.actual_duration_ms = 0; // Will be updated on end
    metrics.viewer_count_at_display = calculate_current_viewer_estimate();
    metrics.completion_percentage = 0.0;
    metrics.premium_user = m_is_premium.load();
    metrics.revenue_share = m_revenue_share.load();
    metrics.content_type = content_type;
    metrics.content_size_bytes = 0; // Could be calculated from content
    
    // Add to display history
    m_analytics.display_history.push_back(metrics);
    
    // Update totals
    m_analytics.total_displays.fetch_add(1);
    
    log_message("ANALYTICS: Ad display started - ID: " + ad_id + ", Campaign: " + campaign_id + 
               ", Planned duration: " + std::to_string(planned_duration_ms) + "ms" +
               ", Viewers: " + std::to_string(metrics.viewer_count_at_display) +
               ", Premium: " + std::string(metrics.premium_user ? "true" : "false") +
               ", Revenue share: " + std::to_string(metrics.revenue_share * 100) + "%");
}

void banner_manager::track_ad_display_end(const std::string& ad_id, int actual_duration_ms)
{
    if (!m_analytics.tracking_enabled.load()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_analytics.analytics_mutex);
    
    // Find the corresponding display metrics
    AdDisplayMetrics* metrics = find_active_ad_display(ad_id);
    if (!metrics) {
        log_message("ANALYTICS: WARNING - Could not find active display for ad ID: " + ad_id);
        return;
    }
    
    // Update metrics
    metrics->end_time = std::chrono::system_clock::now();
    metrics->actual_duration_ms = actual_duration_ms;
    metrics->completion_percentage = calculate_completion_percentage(actual_duration_ms, metrics->planned_duration_ms);
    
    // Update totals
    m_analytics.total_duration_ms.fetch_add(actual_duration_ms);
    
    // Calculate revenue for this display
    double base_revenue = 0.01; // Base revenue per second (configurable)
    double revenue_for_this_display = (actual_duration_ms / 1000.0) * base_revenue * metrics->revenue_share;
    m_analytics.total_revenue.fetch_add(revenue_for_this_display);
    
    log_message("ANALYTICS: Ad display ended - ID: " + ad_id + 
               ", Actual duration: " + std::to_string(actual_duration_ms) + "ms" +
               ", Completion: " + std::to_string(metrics->completion_percentage) + "%" +
               ", Revenue: $" + std::to_string(revenue_for_this_display));
    
    // Send individual display analytics
    send_display_analytics(*metrics);
    
    // Clear current display tracking
    m_current_ad_id.clear();
    m_current_campaign_id.clear();
    m_current_planned_duration.store(0);
}

void banner_manager::track_viewer_metrics(int estimated_viewers, double bitrate, double framerate)
{
    if (!m_analytics.tracking_enabled.load()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_analytics.analytics_mutex);
    
    ViewerMetrics metrics;
    metrics.timestamp = std::chrono::system_clock::now();
    metrics.estimated_viewers = estimated_viewers;
    metrics.twitch_viewers = m_current_twitch_viewers.load();
    metrics.youtube_viewers = m_current_youtube_viewers.load();
    metrics.facebook_viewers = m_current_facebook_viewers.load();
    metrics.stream_bitrate = bitrate;
    metrics.stream_framerate = framerate;
    metrics.stream_quality_score = (framerate >= 30 && bitrate >= 2000) ? 1.0 : 0.5;
    
    // Add to viewer history (keep last 100 entries)
    m_analytics.viewer_history.push_back(metrics);
    if (m_analytics.viewer_history.size() > 100) {
        m_analytics.viewer_history.erase(m_analytics.viewer_history.begin());
    }
    
    // Update estimated total viewers
    m_estimated_total_viewers.store(estimated_viewers);
    m_analytics.average_viewer_count.store(estimated_viewers);
}

void banner_manager::update_platform_viewer_count(const std::string& platform, int viewer_count)
{
    if (platform == "twitch") {
        m_current_twitch_viewers.store(viewer_count);
    } else if (platform == "youtube") {
        m_current_youtube_viewers.store(viewer_count);
    } else if (platform == "facebook") {
        m_current_facebook_viewers.store(viewer_count);
    }
    
    // Recalculate total estimated viewers
    int total = m_current_twitch_viewers.load() + m_current_youtube_viewers.load() + m_current_facebook_viewers.load();
    m_estimated_total_viewers.store(total);
    
    log_message("ANALYTICS: Platform viewer update - " + platform + ": " + std::to_string(viewer_count) + 
               ", Total estimated: " + std::to_string(total));
}

void banner_manager::send_display_analytics(const AdDisplayMetrics& metrics)
{
    // Create analytics report message
    nlohmann::json analytics_msg = {
        {"path", "/api/v1/integration/obs/analytics/ad-display"},
        {"verb", "SET"},
        {"payload", {
            {"ad_id", metrics.ad_id},
            {"campaign_id", metrics.campaign_id},
            {"display_count", 1},
            {"display_duration_ms", metrics.actual_duration_ms},
            {"planned_duration_ms", metrics.planned_duration_ms},
            {"completion_percentage", metrics.completion_percentage},
            {"viewer_count", metrics.viewer_count_at_display},
            {"premium_status", metrics.premium_user ? "true" : "false"},
            {"revenue_share", metrics.revenue_share},
            {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(metrics.start_time.time_since_epoch()).count()},
            {"content_type", metrics.content_type},
            {"stream_metrics", {
                {"bitrate", 0}, // Will be populated from current stream data
                {"framerate", 0},
                {"resolution", "1920x1080"} // Default, could be detected
            }}
        }}
    };
    
    // Send message via WebSocket (need to access obs_plugin instance)
    log_message("ANALYTICS: Sending display analytics for ad: " + metrics.ad_id);
    
    // Send via WebSocket if plugin instance is available
    if (g_obs_plugin_instance && g_obs_plugin_instance->is_connected()) {
        if (g_obs_plugin_instance->send_message(analytics_msg)) {
            log_message("ANALYTICS: Display analytics sent successfully for ad: " + metrics.ad_id);
        } else {
            log_message("ANALYTICS: Failed to send display analytics for ad: " + metrics.ad_id);
        }
    } else {
        log_message("ANALYTICS: WebSocket not connected, logging display analytics: " + analytics_msg.dump());
    }
}

void banner_manager::send_batch_analytics_report()
{
    if (!m_analytics.tracking_enabled.load()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_analytics.analytics_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto report_interval = std::chrono::seconds(m_analytics.report_interval_seconds.load());
    
    // Check if it's time to send a report
    if (now - m_analytics.last_report_time < report_interval) {
        return;
    }
    
    // Calculate report period
    auto period_start = m_analytics.last_report_time;
    auto period_end = now;
    
    // Count displays in this period
    int displays_in_period = 0;
    int total_duration_in_period = 0;
    double total_revenue_in_period = 0.0;
    std::map<std::string, int> campaign_displays;
    
    for (const auto& display : m_analytics.display_history) {
        if (display.start_time >= period_start && display.start_time <= period_end) {
            displays_in_period++;
            total_duration_in_period += display.actual_duration_ms;
            
            // Simple revenue calculation
            double base_revenue = 0.01; // Base revenue per second
            double revenue = (display.actual_duration_ms / 1000.0) * base_revenue * display.revenue_share;
            total_revenue_in_period += revenue;
            
            campaign_displays[display.campaign_id]++;
        }
    }
    
    // Create batch analytics report
    nlohmann::json batch_report = {
        {"path", "/api/v1/integration/obs/analytics/batch-report"},
        {"verb", "SET"},
        {"payload", {
            {"report_period_start", std::chrono::duration_cast<std::chrono::milliseconds>(period_start.time_since_epoch()).count()},
            {"report_period_end", std::chrono::duration_cast<std::chrono::milliseconds>(period_end.time_since_epoch()).count()},
            {"total_ads_displayed", displays_in_period},
            {"total_display_time_ms", total_duration_in_period},
            {"average_viewer_count", m_analytics.average_viewer_count.load()},
            {"premium_user", m_is_premium.load()},
            {"revenue_generated", total_revenue_in_period},
            {"ads_by_campaign", nlohmann::json::array()}
        }}
    };
    
    // Add campaign breakdown
    for (const auto& [campaign_id, count] : campaign_displays) {
        batch_report["payload"]["ads_by_campaign"].push_back({
            {"campaign_id", campaign_id},
            {"displays", count},
            {"total_duration_ms", total_duration_in_period}, // Simplified for now
            {"completion_rate", 95.0} // Placeholder
        });
    }
    
    // Update last report time
    m_analytics.last_report_time = now;
    
    log_message("ANALYTICS: Sending batch report - Displays: " + std::to_string(displays_in_period) + 
               ", Duration: " + std::to_string(total_duration_in_period) + "ms" +
               ", Revenue: $" + std::to_string(total_revenue_in_period));
    
    // Send via WebSocket if plugin instance is available
    if (g_obs_plugin_instance && g_obs_plugin_instance->is_connected()) {
        if (g_obs_plugin_instance->send_message(batch_report)) {
            log_message("ANALYTICS: Batch report sent successfully");
        } else {
            log_message("ANALYTICS: Failed to send batch report");
        }
    } else {
        log_message("ANALYTICS: WebSocket not connected, logging batch report: " + batch_report.dump());
    }
}

void banner_manager::enable_analytics_tracking(bool enable, int report_interval_seconds)
{
    m_analytics.tracking_enabled.store(enable);
    m_analytics.report_interval_seconds.store(report_interval_seconds);
    
    if (enable) {
        m_analytics.last_report_time = std::chrono::system_clock::now();
        log_message("ANALYTICS: Tracking enabled - Report interval: " + std::to_string(report_interval_seconds) + " seconds");
    } else {
        log_message("ANALYTICS: Tracking disabled");
    }
}

banner_manager::AdDisplayMetrics* banner_manager::find_active_ad_display(const std::string& ad_id)
{
    for (auto& display : m_analytics.display_history) {
        if (display.ad_id == ad_id && display.actual_duration_ms == 0) {
            return &display;
        }
    }
    return nullptr;
}

int banner_manager::calculate_current_viewer_estimate()
{
    // Get real OBS streaming data
    obs_output_t* streaming_output = obs_frontend_get_streaming_output();
    if (!streaming_output) {
        // Not streaming, return 0
        return 0;
    }
    
    // Get actual streaming statistics
    uint64_t total_bytes = obs_output_get_total_bytes(streaming_output);
    uint32_t total_frames = obs_output_get_total_frames(streaming_output);
    uint32_t dropped_frames = obs_output_get_frames_dropped(streaming_output);
    // float congestion = obs_output_get_congestion(streaming_output); // Not used currently
    
    // Calculate bitrate (bytes per second)
    static auto start_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    auto duration_seconds = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();
    
    if (duration_seconds > 0) {
        uint64_t bitrate = (total_bytes * 8) / duration_seconds; // bits per second
        
        // Rough viewer estimation based on bitrate
        // Higher bitrate usually means more viewers (streamers increase quality for more viewers)
        int estimated_viewers = 0;
        
        if (bitrate > 0) {
            // Basic estimation: 1 viewer per 1000 bits/second (very rough)
            estimated_viewers = static_cast<int>(bitrate / 1000);
            
            // Apply quality adjustment based on frame drops
            if (dropped_frames > 0 && total_frames > 0) {
                float drop_rate = static_cast<float>(dropped_frames) / total_frames;
                estimated_viewers = static_cast<int>(estimated_viewers * (1.0f - drop_rate));
            }
            
            // Cap at reasonable limits
            estimated_viewers = std::max(1, std::min(estimated_viewers, 100000));
        }
        
        log_message("REAL DATA: Bitrate: " + std::to_string(bitrate) + " bps, " +
                   "Frames: " + std::to_string(total_frames) + ", " +
                   "Dropped: " + std::to_string(dropped_frames) + ", " +
                   "Estimated viewers: " + std::to_string(estimated_viewers));
        
        obs_output_release(streaming_output);
        return estimated_viewers;
    }
    
    obs_output_release(streaming_output);
    
    // Use combined platform viewer counts if available
    int total_viewers = m_estimated_total_viewers.load();
    return total_viewers > 0 ? total_viewers : 0;
}

double banner_manager::calculate_completion_percentage(int actual_duration, int planned_duration)
{
    if (planned_duration <= 0) return 0.0;
    
    double percentage = (static_cast<double>(actual_duration) / planned_duration) * 100.0;
    return std::min(percentage, 100.0); // Cap at 100%
}

// Premium Status and Monetization Functions

void banner_manager::update_premium_status(const nlohmann::json& message)
{
    if (message.contains("premium_status")) {
        std::lock_guard<std::mutex> lock(m_premium_mutex);
        
        bool was_premium = m_is_premium.load();
        bool is_premium = message["premium_status"].get<bool>();
        
        m_is_premium.store(is_premium);
        m_last_premium_update = std::chrono::system_clock::now();
        
        if (is_premium) {
            // Premium user: 80% revenue share
            m_revenue_share.store(0.80f);
            m_custom_positioning.store(true);
            
            // Check for custom ad frequency
            if (message.contains("ad_frequency_minutes")) {
                int custom_frequency = message["ad_frequency_minutes"].get<int>();
                m_ad_frequency_minutes.store(custom_frequency);
                log_message("Premium user - Custom ad frequency: " + std::to_string(custom_frequency) + " minutes");
            } else {
                m_ad_frequency_minutes.store(10);  // Default premium frequency
                log_message("Premium user - Default ad frequency: 10 minutes");
            }
            
            log_message("Premium status ACTIVATED - 80% revenue share, custom positioning enabled");
        } else {
            // Free user: 5% revenue share
            m_revenue_share.store(0.05f);
            m_custom_positioning.store(false);
            m_ad_frequency_minutes.store(5);  // Fixed 5 minutes for free users
            
            log_message("Premium status DEACTIVATED - 5% revenue share, basic positioning only");
        }
        
        // If status changed, log the transition
        if (was_premium != is_premium) {
            std::string transition = was_premium ? "PREMIUM → FREE" : "FREE → PREMIUM";
            log_message("Premium status changed: " + transition);
        }
    }
}

bool banner_manager::is_premium_user() const
{
    return m_is_premium.load();
}

float banner_manager::get_revenue_share() const
{
    return m_revenue_share.load();
}



void banner_manager::start_persistence_monitor()
{
    if (m_persistence_monitor_active) {
        log_message("Persistence monitor already running - ignoring duplicate start request");
        return; // Already running
    }
    
    m_persistence_monitor_active = true;
    
    // Monitor banner visibility and window timeouts with reasonable intervals
    std::jthread([this]() {
        while (m_persistence_monitor_active) {
            bool is_premium = m_is_premium.load();
            
            // ALWAYS check for expired windows first (regardless of user type)
            auto_close_expired_windows();
            
            if (!is_premium) {
                std::this_thread::sleep_for(std::chrono::seconds(2)); // Check more frequently for testing
                if (m_banner_source && !is_banner_visible()) {
                    enforce_banner_visibility();
                }
            } else if (m_banner_persistent) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (m_banner_source && !is_banner_visible()) {
                    enforce_banner_visibility();
                }
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        log_message("Persistence monitor jthread exited");
    }).detach();
    
    bool is_premium = m_is_premium.load();
    if (!is_premium) {
        log_message("FREE USER: Banner monitoring started - Checking every 10 seconds");
    } else {
        log_message("PREMIUM USER: Banner monitoring started - Checking every 15 seconds");
    }
}

void banner_manager::stop_persistence_monitor()
{
    if (m_persistence_monitor_active) {
        m_persistence_monitor_active = false;
        m_banner_persistent = false;
        log_message("Persistence monitor stopped");
    }
}

void banner_manager::enforce_banner_visibility()
{
    // CRITICAL: Don't enforce during intentional hide operations
    if (m_intentional_hide_in_progress.load()) {
        log_message("PERSISTENCE: Skipping enforcement - intentional hide in progress");
        return;
    }
    
    // Don't enforce if banner is supposed to be hidden due to frequency limits
    if (!m_is_premium.load() && !can_show_ad_now()) {
        log_message("PERSISTENCE: Skipping enforcement - ad frequency limit active");
        return;
    }
    
    bool is_premium = m_is_premium.load();
    
    if (!is_premium) {
        // FREE USERS: Always enforce banner visibility (when not intentionally hidden)
    } else {
        // PREMIUM USERS: Complete freedom - no enforcement unless they explicitly enable persistence
        if (!m_banner_persistent) {
            return;  // Premium users have complete freedom
        }
        log_message("PREMIUM USER: Persistence mode enabled - checking banner visibility");
    }
    
    // Recreate banner source if it was destroyed
    if (!m_banner_source && !m_current_banner_content.empty()) {
        log_message("Banner source was destroyed! Recreating it...");
        create_banner_source(m_current_banner_content, m_current_content_type);
    }
    
    if (!m_banner_source) {
        return;
    }
    
    // Check all scenes and enforce banner visibility
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    bool banner_found = false;
    bool action_taken = false;
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* scene_source = scenes.sources.array[i];
        if (!scene_source) continue;
        
        obs_scene_t* scene = obs_scene_from_source(scene_source);
        if (!scene) continue;
        
        // Use metadata-based detection instead of name-based
        obs_sceneitem_t* banner_item = find_vortideck_banner_in_scene(scene);
        if (banner_item) {
            banner_found = true;
            
            // Check if banner is hidden and restore visibility
            if (!obs_sceneitem_visible(banner_item)) {
                obs_sceneitem_set_visible(banner_item, true);
                action_taken = true;
                if (!is_premium) {
                    log_message("FREE USER: Banner was hidden - restored to visible!");
                } else {
                    log_message("PREMIUM USER: Banner restored to visible (persistence mode)");
                }
            }
            
            // For FREE USERS: Always enforce strict locking and positioning
            if (!is_premium) {
                lock_banner_item(banner_item);
            } else {
                // For PREMIUM USERS: Only basic locking check
                if (!obs_sceneitem_locked(banner_item)) {
                    obs_sceneitem_set_locked(banner_item, true);
                    log_message("Re-locked banner that was unlocked");
                }
            }
            
            // Keep banner on top
            obs_sceneitem_set_order(banner_item, OBS_ORDER_MOVE_TOP);
        } else {
            // Only recreate in current scene to prevent spam across all scenes
            obs_source_t* current_scene_source = obs_frontend_get_current_scene();
            if (current_scene_source && scene_source == current_scene_source) {
            if (!is_premium) {
                    log_message("FREE USER: Banner missing in current scene - recreating");
                
                obs_sceneitem_t* scene_item = obs_scene_add(scene, m_banner_source);
                if (scene_item) {
                    // Position banner at 0,0 to fill entire screen (CSS controls content positioning)
                    vec2 pos;
                    pos.x = 0;
                    pos.y = 0; // Top-left corner (0,0)
                    obs_sceneitem_set_pos(scene_item, &pos);
                    
                    // Set size to fill entire screen
                    vec2 scale;
                    scale.x = 1.0f;
                    scale.y = 1.0f;
                    obs_sceneitem_set_scale(scene_item, &scale);
                    
                        // Make it visible and properly locked
                    obs_sceneitem_set_visible(scene_item, true);
                    lock_banner_item(scene_item);
                    obs_sceneitem_set_order(scene_item, OBS_ORDER_MOVE_TOP);
                    
                        action_taken = true;
                        log_message("FREE USER: Banner restored in current scene");
                }
            } else if (m_banner_persistent) {
                log_message("PREMIUM USER: Banner missing but in persistent mode - recreating");
                
                obs_sceneitem_t* scene_item = obs_scene_add(scene, m_banner_source);
                if (scene_item) {
                    obs_sceneitem_set_visible(scene_item, true);
                    obs_sceneitem_set_locked(scene_item, true);
                    obs_sceneitem_set_order(scene_item, OBS_ORDER_MOVE_TOP);
                        action_taken = true;
                }
            }
            }
            if (current_scene_source) {
                obs_source_release(current_scene_source);
            }
        }
    }
    
    obs_frontend_source_list_free(&scenes);
    
    // Log enforcement actions
    if (action_taken) {
        if (!is_premium) {
            log_message("FREE USER: Banner protection enforced - upgrade to premium for full control");
        } else {
            log_message("PREMIUM USER: Banner visibility enforced due to persistence mode");
        }
    }
}

void banner_manager::force_refresh_banner_visibility()
{
    // Skip if intentional hide is in progress
    if (m_intentional_hide_in_progress.load()) {
        return;
    }
    
    // Force refresh all banners to ensure they're truly visible
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    int refreshed_banners = 0;
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* scene_source = scenes.sources.array[i];
        if (!scene_source) continue;
        
        obs_scene_t* scene = obs_scene_from_source(scene_source);
        if (!scene) continue;
        
        // Find any VortiDeck banners in this scene
        obs_sceneitem_t* banner_item = find_vortideck_banner_in_scene(scene);
        if (banner_item) {
            // Double-check visibility and force refresh
            bool is_visible = obs_sceneitem_visible(banner_item);
            if (!is_visible) {
                obs_sceneitem_set_visible(banner_item, true);
                refreshed_banners++;
                
                const char* scene_name = obs_source_get_name(scene_source);
                log_message("FORCE_REFRESH: Made banner visible in scene '" + std::string(scene_name) + "'");
            }
        }
    }
    
    obs_frontend_source_list_free(&scenes);
    
    // Only log if we actually did something
    if (refreshed_banners > 0) {
        log_message("FORCE_REFRESH: Refreshed " + std::to_string(refreshed_banners) + " banners");
    }
}

// ============================================================================
// OBS SOURCE REGISTRATION AND CALLBACKS
// ============================================================================

void banner_manager::register_vortideck_banner_source()
{
    // Register VortiDeck Banner as a custom source type that creates browser sources internally
    struct obs_source_info vortideck_banner_info = {};
    vortideck_banner_info.id = "vortideck_banner";
    vortideck_banner_info.type = OBS_SOURCE_TYPE_INPUT;
    vortideck_banner_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    
    vortideck_banner_info.get_name = [](void* unused) -> const char* {
        UNUSED_PARAMETER(unused);
        return "VortiDeck Banner";
    };
    
    vortideck_banner_info.create = [](obs_data_t* settings, obs_source_t* source) -> void* {
        // Create a browser source internally
        obs_data_t* browser_settings = obs_data_create();
        
        // Get settings from our custom source
        const char* url = obs_data_get_string(settings, "url");
        const char* css = obs_data_get_string(settings, "css");
        int width = obs_data_get_int(settings, "width");
        int height = obs_data_get_int(settings, "height");
        
        // Set default values if not provided
        if (!url || strlen(url) == 0) {
            url = "data:text/html,<html><body><h2>VortiDeck Banner</h2></body></html>";
        }
        if (width == 0) width = 1920;
        if (height == 0) height = 100;
        
        // Configure browser source
        obs_data_set_string(browser_settings, "url", url);
        obs_data_set_string(browser_settings, "css", css ? css : "");
        obs_data_set_int(browser_settings, "width", width);
        obs_data_set_int(browser_settings, "height", height);
        obs_data_set_int(browser_settings, "fps", 30);
        obs_data_set_bool(browser_settings, "reroute_audio", false);
        obs_data_set_bool(browser_settings, "restart_when_active", true);
        obs_data_set_bool(browser_settings, "shutdown", false);
        
        // Create the browser source
        obs_source_t* browser_source = obs_source_create("browser_source", obs_source_get_name(source), browser_settings, nullptr);
        obs_data_release(browser_settings);
        
        if (browser_source) {
            // Add VortiDeck metadata
            obs_data_t* private_settings = obs_source_get_private_settings(browser_source);
    obs_data_set_string(private_settings, "vortideck_banner", "true");
    obs_data_set_string(private_settings, "vortideck_banner_id", "banner_v1");
            obs_data_set_string(private_settings, "vortideck_banner_type", "browser");
    obs_data_release(private_settings);
    
            log_to_obs("VortiDeck Banner: Created browser source wrapper");
        }
        
        return browser_source;  // Return the browser source as our internal data
    };
    
    vortideck_banner_info.destroy = [](void* data) {
        if (data) {
            obs_source_t* browser_source = (obs_source_t*)data;
            obs_source_release(browser_source);
        }
    };
    
    vortideck_banner_info.update = [](void* data, obs_data_t* settings) {
        if (data) {
            obs_source_t* browser_source = (obs_source_t*)data;
            
            // Update browser source settings
            obs_data_t* browser_settings = obs_data_create();
            
            const char* url = obs_data_get_string(settings, "url");
            const char* css = obs_data_get_string(settings, "css");
            int width = obs_data_get_int(settings, "width");
            int height = obs_data_get_int(settings, "height");
            
            if (!url || strlen(url) == 0) {
                url = "data:text/html,<html><body><h2>VortiDeck Banner</h2></body></html>";
            }
            if (width == 0) width = 1920;
            if (height == 0) height = 100;
            
            obs_data_set_string(browser_settings, "url", url);
            obs_data_set_string(browser_settings, "css", css ? css : "");
            obs_data_set_int(browser_settings, "width", width);
            obs_data_set_int(browser_settings, "height", height);
            obs_data_set_int(browser_settings, "fps", 30);
            obs_data_set_bool(browser_settings, "reroute_audio", false);
            obs_data_set_bool(browser_settings, "restart_when_active", true);
            obs_data_set_bool(browser_settings, "shutdown", false);
            
            obs_source_update(browser_source, browser_settings);
            obs_data_release(browser_settings);
            
            log_to_obs("VortiDeck Banner: Updated browser source settings");
        }
    };
    
    vortideck_banner_info.get_defaults = [](obs_data_t* settings) {
        obs_data_set_default_string(settings, "url", "data:text/html,<html><body><h2>VortiDeck Banner</h2></body></html>");
        obs_data_set_default_string(settings, "css", "body { margin: 0; padding: 0; background: transparent; width: 100vw; height: 100vh; overflow: hidden; }");
    obs_data_set_default_int(settings, "width", 1920);
    obs_data_set_default_int(settings, "height", 100);
    };

    vortideck_banner_info.get_properties = [](void* data) -> obs_properties_t* {
    UNUSED_PARAMETER(data);
    
    obs_properties_t* props = obs_properties_create();
    
        obs_properties_add_text(props, "url", "Content URL", OBS_TEXT_DEFAULT);
        obs_properties_add_text(props, "css", "Custom CSS", OBS_TEXT_MULTILINE);
        obs_properties_add_int(props, "width", "Width", 100, 4096, 1);
        obs_properties_add_int(props, "height", "Height", 50, 2160, 1);
    
    return props;
    };
    
    vortideck_banner_info.video_render = [](void* data, gs_effect_t* /*effect*/) {
        if (data) {
            obs_source_t* browser_source = (obs_source_t*)data;
            obs_source_video_render(browser_source);
        }
    };
    
    vortideck_banner_info.get_width = [](void* data) -> uint32_t {
        if (data) {
            obs_source_t* browser_source = (obs_source_t*)data;
            return obs_source_get_width(browser_source);
        }
        return 1920;
    };
    
    vortideck_banner_info.get_height = [](void* data) -> uint32_t {
        if (data) {
            obs_source_t* browser_source = (obs_source_t*)data;
            return obs_source_get_height(browser_source);
        }
        return 100;
    };
    
    obs_register_source(&vortideck_banner_info);
    log_to_obs("VortiDeck Banner: Registered as custom source (creates browser sources internally)");
}

void banner_manager::unregister_vortideck_banner_source()
{
    // OBS automatically handles unregistration when the module unloads
    log_to_obs("VortiDeck Banner: Source unregistered");
}

// Custom source callbacks removed - using browser_source directly for full compatibility

// ============================================================================
// BANNER QUEUE MANAGEMENT IMPLEMENTATION
// ============================================================================

void banner_manager::set_ad_queue(const std::vector<AdWindow>& queue)
{
    std::lock_guard<std::mutex> lock(m_ad_queue.queue_mutex);
    
    // Stop current rotation
    stop_ad_rotation();
    
    // Clear existing queue and set new one
    m_ad_queue.banners = queue;
    m_ad_queue.current_index.store(0);
    
    log_message("QUEUE: Set new ad queue with " + std::to_string(queue.size()) + " banners");
    
    // Log queue contents
    for (size_t i = 0; i < queue.size(); ++i) {
        const auto& ad = queue[i];
        log_message("QUEUE[" + std::to_string(i) + "]: " + ad.id + " (" + 
                   std::to_string(ad.duration_seconds) + "s) - " + ad.content_type);
    }
    
    // Start rotation if queue has banners and auto-rotation is enabled
    if (!queue.empty() && m_ad_queue.auto_rotation.load()) {
        start_ad_rotation();
    }
}

void banner_manager::add_ad_to_queue(const AdWindow& ad)
{
    bool should_start_rotation = false;
    
    {
        std::lock_guard<std::mutex> lock(m_ad_queue.queue_mutex);
        
        m_ad_queue.banners.push_back(ad);
        
        log_message("QUEUE: Added banner '" + ad.id + "' to queue (total: " + 
                   std::to_string(m_ad_queue.banners.size()) + ")");
        
        // DEBUG: Log queue state
        log_message("QUEUE: Auto-rotation enabled: " + std::string(m_ad_queue.auto_rotation.load() ? "true" : "false"));
        log_message("QUEUE: Rotation active: " + std::string(m_rotation_active.load() ? "true" : "false"));
        log_message("QUEUE: Banner details - Duration: " + std::to_string(ad.duration_seconds) + "s, Content: " + ad.content_data.substr(0, 50) + "...");
        
        // Check if we should start rotation (do this while still holding the lock)
        should_start_rotation = m_ad_queue.auto_rotation.load() && !m_rotation_active.load();
        
        if (should_start_rotation) {
            log_message("QUEUE: Will start rotation - auto-rotation enabled, rotation not active");
        } else {
            log_message("QUEUE: Not starting rotation - Auto-rotation: " + std::string(m_ad_queue.auto_rotation.load() ? "true" : "false") + 
                       ", Rotation active: " + std::string(m_rotation_active.load() ? "true" : "false"));
        }
    } // Release the mutex by ending the scope
    
    // NOW start rotation outside the mutex lock to prevent deadlock
    if (should_start_rotation) {
        log_message("QUEUE: Starting rotation outside mutex lock");
        start_ad_rotation();
    }
}

void banner_manager::start_ad_rotation()
{
    log_message("ROTATION: start_ad_rotation() called");
    
    if (m_rotation_active.load()) {
        log_message("ROTATION: Already active, ignoring start request");
        return;
    }
    
    // Check if queue has banners WITHOUT holding mutex (caller already checked)
    size_t queue_size = 0;
    std::string first_banner_id;
    int first_banner_duration = 0;
    
    {
        std::lock_guard<std::mutex> lock(m_ad_queue.queue_mutex);
        if (m_ad_queue.banners.empty()) {
            log_message("ROTATION: No banners in queue, cannot start rotation");
            return;
        }
        queue_size = m_ad_queue.banners.size();
        first_banner_id = m_ad_queue.banners[0].id;
        first_banner_duration = m_ad_queue.banners[0].duration_seconds;
    } // Release mutex before starting thread
    
    m_rotation_active.store(true);
    
    log_message("ROTATION: Starting rotation with " + std::to_string(queue_size) + " banners");
    log_message("ROTATION: First banner ID: " + first_banner_id + ", Duration: " + std::to_string(first_banner_duration) + "s");
    
    // Start rotation thread with proper window management
    m_rotation_jthread = std::jthread([this](std::stop_token stop_token) {
        log_message("ROTATION: Rotation thread started with window management");
        
        while (!stop_token.stop_requested()) {
            // STEP 1: Wait for gap to end if we're in one
            if (is_in_window_gap()) {
                log_message("ROTATION: Waiting for window gap to end...");
                while (is_in_window_gap() && !stop_token.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                if (stop_token.stop_requested()) break;
                log_message("ROTATION: Window gap ended - can start new window");
                
                // Ensure free users still have visible banner after gap
                if (!is_premium_user()) {
                    log_message("ROTATION: FREE USER - Preparing banner for new window");
                    // The banner source should still be visible (transparent content during gap)
                    // Just continue to show ads in the new window
                }
            }
            
            // STEP 2: Start new ad window (10 seconds max)
            if (can_start_new_window()) {
                start_new_window();
                log_message("ROTATION: Started new 10-second ad window");
                
                // Wait a moment for window to properly initialize
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                log_message("ROTATION: Cannot start new window - waiting...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            
            // STEP 3: Show ads within the 10-second window
            int window_time_used = 0;
            const int max_window_time = 10; // 10 seconds for testing
            std::set<std::string> displayed_ads_in_window; // Track all displayed ads in this window
            size_t ads_displayed_in_window = 0;
            
            while (window_time_used < max_window_time && !stop_token.stop_requested() && m_window_active.load()) {
                // Get next ad from queue
                std::string current_ad_id;
                int ad_duration = 0;
                bool has_ad = false;
                
                {
                    std::lock_guard<std::mutex> queue_lock(m_ad_queue.queue_mutex);
                    if (!m_ad_queue.banners.empty()) {
                        size_t current_idx = m_ad_queue.current_index.load();
                        if (current_idx >= m_ad_queue.banners.size()) {
                            current_idx = 0;
                            m_ad_queue.current_index.store(current_idx);
                        }
                        
                        const auto& current_ad = m_ad_queue.banners[current_idx];
                        current_ad_id = current_ad.id;
                        ad_duration = current_ad.duration_seconds;
                        
                        // Smart rotation logic:
                        // 1. If we have only 1 ad, show it once per window
                        // 2. If we have multiple ads, show each once per window
                        // 3. If we've shown all ads in this window, close the window
                        
                        if (m_ad_queue.banners.size() == 1) {
                            // Single ad case: show once per window
                            if (displayed_ads_in_window.count(current_ad_id) > 0) {
                                log_message("ROTATION: Single ad already displayed in this window - closing window");
                                break;
                            }
                        } else {
                            // Multiple ads case: show each ad once per window
                            if (displayed_ads_in_window.size() >= m_ad_queue.banners.size()) {
                                log_message("ROTATION: All ads displayed in this window - closing window");
                                break;
                            }
                            
                            // Skip if this specific ad was already shown in this window
                            if (displayed_ads_in_window.count(current_ad_id) > 0) {
                                log_message("ROTATION: Ad " + current_ad_id + " already displayed in this window - rotating to next");
                                rotate_to_next_ad();
                                continue;
                            }
                        }
                        
                        // Check if ad can fit in remaining window time
                        int remaining_time = max_window_time - window_time_used;
                        if (ad_duration <= remaining_time) {
                            has_ad = true;
                            
                            log_message("ROTATION: Showing ad " + current_ad_id + " (" + 
                                       std::to_string(ad_duration) + "s) in window [" + 
                                       std::to_string(ads_displayed_in_window + 1) + "/" + 
                                       std::to_string(m_ad_queue.banners.size()) + "]");
                            
                            // Set the banner content
                            set_banner_content_with_custom_params(
                                current_ad.content_data,
                                current_ad.content_type,
                                current_ad.css,
                                0, 0, false
                            );
                            
                            // Show the banner with correct duration (rotation manages timing)
                            if (is_premium_user()) {
                                show_premium_banner();
                            } else {
                                show_banner(false); // Don't start duration timer - rotation manages it
                            }
                            
                            // CRITICAL: Start duration timer with queue duration (not default 60s)
                            log_message("ROTATION: Starting duration timer for " + std::to_string(ad_duration) + " seconds (from queue)");
                            start_duration_timer(ad_duration);
                            
                            // Track analytics
                            track_ad_display_start(current_ad_id, "queue_campaign", 
                                                 ad_duration * 1000, current_ad.content_type);
                            
                            // Mark this ad as displayed in this window
                            displayed_ads_in_window.insert(current_ad_id);
                            ads_displayed_in_window++;
                        } else {
                            log_message("ROTATION: Ad " + current_ad_id + " (" + 
                                       std::to_string(ad_duration) + "s) too long for remaining window time (" + 
                                       std::to_string(remaining_time) + "s) - closing window");
                            break; // Close window and start gap
                        }
                    }
                }
                
                if (!has_ad) {
                    log_message("ROTATION: No ads available - closing window");
                    break;
                }
                
                // Wait for ad duration
                for (int i = 0; i < ad_duration && !stop_token.stop_requested(); ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                
                // Track analytics end
                track_ad_display_end(current_ad_id, ad_duration * 1000);
                
                // Update window time used
                window_time_used += ad_duration;
                m_current_window_used_time.store(window_time_used);
                
                // Move to next ad
                rotate_to_next_ad();
                
                log_message("ROTATION: Window time used: " + std::to_string(window_time_used) + 
                           "/" + std::to_string(max_window_time) + " seconds");
            }
            
            // STEP 4: Close window and start gap (only if still active)
            if (m_window_active.load()) {
                log_message("ROTATION: Window time expired - closing window and starting gap");
                close_current_window(); // This will start the 15-second gap
                
                // Handle gap period based on user type
                hide_banner_with_user_restrictions("window gap period");
            } else {
                log_message("ROTATION: Window already closed (likely by duration timer)");
            }
        }
        
        log_message("ROTATION: Rotation thread stopped");
        m_rotation_active.store(false);
    });
}

void banner_manager::stop_ad_rotation()
{
    if (!m_rotation_active.load()) {
        return;
    }
    
    log_message("ROTATION: Stopping banner rotation");
    
    // Request stop using jthread's built-in mechanism
    m_rotation_jthread.request_stop();
    
    // jthread automatically joins in destructor, but we can manually join if needed
    if (m_rotation_jthread.joinable()) {
        m_rotation_jthread.join();
    }
    
    m_rotation_active.store(false);
    log_message("ROTATION: Banner rotation stopped");
}

void banner_manager::rotate_to_next_ad()
{
    std::lock_guard<std::mutex> lock(m_ad_queue.queue_mutex);
    
    if (m_ad_queue.banners.empty()) {
        return;
    }
    
    size_t current_idx = m_ad_queue.current_index.load();
    size_t next_idx = (current_idx + 1) % m_ad_queue.banners.size();
    
    m_ad_queue.current_index.store(next_idx);
    
    log_message("ROTATION: Advanced to banner " + std::to_string(next_idx + 1) + "/" + 
               std::to_string(m_ad_queue.banners.size()));
}

bool banner_manager::is_current_ad_expired() const
{
    // Cast away const for mutex - safe since we're only reading data
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_ad_queue.queue_mutex));
    
    if (m_ad_queue.banners.empty()) {
        return false;
    }
    
    size_t current_idx = m_ad_queue.current_index.load();
    if (current_idx >= m_ad_queue.banners.size()) {
        return true;
    }
    
    const auto& current_ad = m_ad_queue.banners[current_idx];
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - current_ad.start_time);
    
    return elapsed.count() >= current_ad.duration_seconds;
}

void banner_manager::set_ad_duration_limit(int max_seconds)
{
    m_max_ad_duration.store(max_seconds);
    log_message("DURATION: Set maximum ad duration to " + std::to_string(max_seconds) + " seconds");
}

// Auto-rotation control
void banner_manager::enable_auto_rotation(bool enable)
{
    m_ad_queue.auto_rotation.store(enable);
    log_message("AUTO_ROTATION: " + std::string(enable ? "Enabled" : "Disabled") + " auto-rotation");
    
    // Start rotation if enabled and queue has banners
    if (enable) {
        std::lock_guard<std::mutex> lock(m_ad_queue.queue_mutex);
        if (!m_ad_queue.banners.empty() && !m_rotation_active.load()) {
            start_ad_rotation();
        }
    } else {
        // Stop rotation if disabled
        stop_ad_rotation();
    }
}

bool banner_manager::is_auto_rotation_enabled() const
{
    return m_ad_queue.auto_rotation.load();
}

// Window lifecycle methods
void banner_manager::start_ad_window(std::string_view window_id)
{
    m_current_window_start = std::chrono::system_clock::now();
    m_current_window_used_time.store(0);
    m_window_active.store(true);
    
    log_message("WINDOW: Started ad window '" + std::string(window_id) + "'");
}

void banner_manager::close_ad_window(std::string_view window_id)
{
    m_window_active.store(false);
    
    int used_time = m_current_window_used_time.load();
    log_message("WINDOW: Closed ad window '" + std::string(window_id) + "' - Used time: " + 
               std::to_string(used_time) + " seconds");
    
    // Send window summary analytics
    if (g_obs_plugin_instance && g_obs_plugin_instance->is_connected()) {
        nlohmann::json window_report = {
            {"path", "/api/v1/obs/analytics/window-closed"},
            {"verb", "SET"},
            {"payload", {
                {"window_id", std::string(window_id)},
                {"total_time_used", used_time},
                {"window_end_time", std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()}
            }}
        };
        
        g_obs_plugin_instance->send_message(window_report);
    }
}

void banner_manager::auto_close_expired_windows()
{
    if (!m_window_active.load()) {
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    auto window_duration = std::chrono::duration_cast<std::chrono::seconds>(now - m_current_window_start);
    
    // TESTING: Close window after 10 seconds (normally 40 seconds)
    if (window_duration.count() > 10) {
        log_message("WINDOW_AUTO_CLOSE: Force closing window after " + std::to_string(window_duration.count()) + " seconds");
        close_ad_window("auto_expired_window_10s");
    }
}

// Duration control helpers
int banner_manager::get_effective_ad_duration(int requested_duration) const
{
    int max_duration = get_max_duration_for_user_type();
    return std::min(requested_duration, max_duration);
}

int banner_manager::get_max_duration_for_user_type() const
{
    if (is_premium_user()) {
        return 300; // 5 minutes for premium users
    } else {
        return 60;  // 1 minute for free users
    }
}

bool banner_manager::should_cap_ad_duration(int requested_duration) const
{
    return requested_duration > get_max_duration_for_user_type();
}

// Window management
bool banner_manager::can_ad_fit_in_current_window(int ad_duration) const
{
    if (!m_window_active.load()) {
        return true; // No window restrictions if window not active
    }
    
    int used_time = m_current_window_used_time.load();
    // TESTING: 10 seconds window (normally 40 seconds)
    // PRODUCTION: Change to 40 seconds
    int max_window_time = 10; // 10 seconds for testing
    
    return (used_time + ad_duration) <= max_window_time;
}

void banner_manager::close_current_window()
{
    if (m_window_active.load()) {
        close_ad_window("manual_close");
        // Start gap period after closing window
        start_window_gap();
    }
}

void banner_manager::start_new_window()
{
    if (m_window_active.load()) {
        close_current_window();
        return; // Window closed, gap started - new window will be started after gap
    }
    
    if (!can_start_new_window()) {
        log_message("WINDOW: Cannot start new window - still in gap period");
        return;
    }
    
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    start_ad_window("window_" + std::to_string(timestamp));
}

int banner_manager::get_remaining_window_time() const
{
    if (!m_window_active.load()) {
        return 10; // 10 seconds for testing window
    }
    
    int used_time = m_current_window_used_time.load();
    // TESTING: 10 seconds window (normally 40 seconds)
    int max_window_time = 10; // 10 seconds for testing
    
    return std::max(0, max_window_time - used_time);
}

void banner_manager::reset_window_tracking()
{
    m_current_window_used_time.store(0);
    m_window_active.store(false);
    
    log_message("WINDOW: Reset window tracking");
}

// Gap management between windows
void banner_manager::start_window_gap()
{
    if (m_in_window_gap.load()) {
        log_message("WINDOW_GAP: Already in gap period - ignoring duplicate start");
        return;
    }
    
    m_in_window_gap.store(true);
    m_window_gap_start = std::chrono::system_clock::now();
    
    log_message("WINDOW_GAP: Starting 15-second gap between windows (testing mode)");
    
    // Start gap timer thread
    m_window_gap_thread = std::jthread([this](std::stop_token stop_token) {
        // TESTING: 15 seconds gap (normally 3 minutes = 180 seconds)
        const int gap_seconds = 15; // 15 seconds for testing
        
        for (int i = 0; i < gap_seconds && !stop_token.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (!stop_token.stop_requested()) {
            log_message("WINDOW_GAP: Gap period ended - can start new window");
            m_in_window_gap.store(false);
        }
    });
}

void banner_manager::stop_window_gap()
{
    if (m_in_window_gap.load()) {
        log_message("WINDOW_GAP: Stopping gap period");
        m_window_gap_thread.request_stop();
        if (m_window_gap_thread.joinable()) {
            m_window_gap_thread.join();
        }
        m_in_window_gap.store(false);
    }
}

bool banner_manager::is_in_window_gap() const
{
    return m_in_window_gap.load();
}

bool banner_manager::can_start_new_window() const
{
    return !m_window_active.load() && !m_in_window_gap.load();
}

void banner_manager::set_custom_banner_duration(int duration_seconds)
{
    m_custom_banner_duration.store(duration_seconds);
    log_message("DURATION: Set custom banner duration to " + std::to_string(duration_seconds) + " seconds");
}

bool banner_manager::can_show_ad_now() const
{
    if (m_is_premium.load()) {
        return true; // Premium users have no frequency restrictions
    }
    
    // WINDOW SYSTEM: If we're in an active window, ignore frequency limits
    if (m_window_active.load()) {
        const_cast<banner_manager*>(this)->log_message("FREQUENCY: In active window - frequency limits bypassed");
        return true;
    }
    
    // WINDOW SYSTEM: If we're in gap period, no ads allowed
    if (m_in_window_gap.load()) {
        const_cast<banner_manager*>(this)->log_message("FREQUENCY: In window gap - no ads allowed");
        return false;
    }
    
    // Free users must wait between WINDOWS (not individual ads)
    auto now = std::chrono::system_clock::now();
    auto time_since_last_ad = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_ad_end_time);
    
    const int min_frequency_seconds = 30; // 30 seconds between windows
    
    // DEBUG: Log frequency check details (use const_cast for logging in const method)
    const_cast<banner_manager*>(this)->log_message("FREQUENCY: Time since last window: " + std::to_string(time_since_last_ad.count()) + "s, Required: " + std::to_string(min_frequency_seconds) + "s");
    
    bool can_show = time_since_last_ad.count() >= min_frequency_seconds;
    const_cast<banner_manager*>(this)->log_message("FREQUENCY: Can start new window: " + std::string(can_show ? "true" : "false"));
    
    return can_show;
}

void banner_manager::start_duration_timer(int duration_seconds)
{
    // Stop any existing timer first
    stop_duration_timer();
    
    m_duration_timer_active.store(true);
    log_message("TIMER: Starting banner duration timer for " + std::to_string(duration_seconds) + " seconds");
    
    m_duration_timer_thread = std::jthread([this, duration_seconds](std::stop_token stop_token) {
        // Wait for the specified duration
        for (int i = 0; i < duration_seconds && !stop_token.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (!stop_token.stop_requested()) {
            log_message("TIMER: Banner duration expired (" + std::to_string(duration_seconds) + "s) - auto-hiding");
            
            // Set intentional hide flag to prevent interference
            m_intentional_hide_in_progress.store(true);
            
            // CRITICAL: Record ad end time BEFORE hiding to prevent auto-restore
            m_last_ad_end_time = std::chrono::system_clock::now();
            
            // NOTE: Don't update window_used_time here - rotation thread handles this
            
            // Handle banner based on user type (no auto-restore for duration timer expiration)
            hide_banner_with_user_restrictions("duration timer expiration");
            
            // Close window (this will also start the gap period)
            close_current_window();
            
            // Reset custom duration for next banner
            m_custom_banner_duration.store(0);
            
            log_message("TIMER: Banner permanently hidden, window closed, ad end time recorded");
            
            // Wait a bit to ensure hide is complete
            std::this_thread::sleep_for(std::chrono::seconds(2));
            
            // Clear intentional hide flag
            m_intentional_hide_in_progress.store(false);
        }
        
        m_duration_timer_active.store(false);
    });
}

void banner_manager::stop_duration_timer()
{
    if (m_duration_timer_active.load()) {
        log_message("TIMER: Stopping existing banner duration timer");
        m_duration_timer_thread.request_stop();
        if (m_duration_timer_thread.joinable()) {
            m_duration_timer_thread.join();
        }
        m_duration_timer_active.store(false);
    }
}

// Helper function to handle banner hiding with user type restrictions
void banner_manager::hide_banner_with_user_restrictions(const std::string& reason)
{
    if (is_premium_user()) {
        // PREMIUM: Can hide banners
        log_message("BANNER_HIDE: PREMIUM USER - Hiding banner (" + reason + ")");
        remove_banner_from_scenes();
        m_banner_visible = false;
    } else {
        // FREE: Show transparent content instead of hiding (maintain banner source visibility)
        log_message("BANNER_HIDE: FREE USER - Showing transparent content (" + reason + ") - cannot hide banners");
        set_banner_content("transparent", "transparent");
        // Keep m_banner_visible = true for free users (banner source remains visible)
    }
}

