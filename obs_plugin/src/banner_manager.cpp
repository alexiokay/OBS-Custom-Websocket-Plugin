#include "banner_manager.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <set>
#include <nlohmann/json.hpp>
#include "vortideck_common.h"

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
    
    // CRITICAL: Initialize FREE USER mode as DEFAULT
    m_is_premium.store(false);  // FREE USER by default
    m_custom_positioning.store(false);  // No custom positioning for free users
    
    log_message("CONSTRUCTOR: Banner manager initialized - FREE USER MODE");
    log_message("CONSTRUCTOR: FREE USER - FORCED banner system - banners MUST be present in ALL scenes");
    log_message("CONSTRUCTOR: FREE USER - Limited banner control - upgrade to premium for complete banner freedom");
    log_message("CONSTRUCTOR: FREE USER - Banners auto-restore after hiding");
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
    log_message("Banner manager destructor called...");

    // Set shutdown flag
    m_shutting_down = true;

    // CRITICAL: Stop polling thread FIRST before any cleanup
    if (m_polling_thread.joinable()) {
        log_message("DESTRUCTOR: Requesting polling thread to stop...");
        m_polling_thread.request_stop();
        // jthread will automatically join on destruction
    }

    // Clean up signal handlers and monitoring
    disconnect_scene_signals();
    disconnect_source_signals();
    stop_persistence_monitor();

    // Release single shared banner source
    if (m_banner_source) {
        obs_source_release(m_banner_source);
        m_banner_source = nullptr;
    }

    log_message("Banner manager destroyed - polling thread automatically joined");
}

void banner_manager::set_shutting_down()
{
    log_message("SHUTDOWN FLAG SET - All signal handlers will now abort immediately");
    m_shutting_down = true;
}

void banner_manager::disconnect_all_signals()
{
    log_message("Disconnecting ALL banner_manager signals (called from obs_module_unload)");
    disconnect_scene_signals();
    disconnect_source_signals();
    log_message("All signals disconnected successfully");
}

void banner_manager::shutdown()
{
    log_message("Banner manager shutdown requested...");

    // CRITICAL: Set shutdown flag FIRST
    m_shutting_down = true;

    // CRITICAL: Stop polling thread BEFORE any OBS API cleanup
    // This prevents race condition where thread calls OBS API during CEF shutdown
    if (m_polling_thread.joinable()) {
        log_message("SHUTDOWN: Stopping polling thread...");
        m_polling_thread.request_stop();  // Signal thread to stop

        // jthread will automatically join on destruction
        // The thread should exit quickly due to stop_token.stop_requested() check
        log_message("SHUTDOWN: Polling thread stop requested - will auto-join on destruction");
    } else {
        log_message("SHUTDOWN: No polling thread to stop");
    }

    // DO NOT manually destroy banner source - let OBS handle it during obs_shutdown()
    // Manually destroying causes OBS to hang and browser processes to remain
    // The polling thread being stopped is sufficient to prevent crashes
    log_message("SHUTDOWN: Leaving banner source for OBS to clean up naturally");

    stop_persistence_monitor();  // Stop any persistence monitoring

    log_message("Banner manager shutdown complete - polling thread stopped, OBS can proceed with cleanup");
}

void banner_manager::initialize_after_obs_ready()
{
    log_message("INITIALIZATION: Starting banner initialization process...");

    // CRITICAL: Check shutdown flag FIRST - don't initialize during shutdown
    if (m_shutting_down.load()) {
        log_message("INITIALIZATION: Shutdown in progress - aborting initialization");
        return;
    }

    // Safety check - ensure OBS is fully initialized
    if (!obs_frontend_get_current_scene()) {
        log_message("INITIALIZATION: OBS not fully initialized yet - delaying banner initialization");

        // Try again after a short delay
        std::jthread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // CRITICAL: Check shutdown flag before retrying initialization
            if (m_shutting_down.load()) {
                log_message("INITIALIZATION: Shutdown detected, aborting delayed initialization");
                return;
            }

            initialize_after_obs_ready();
        }).detach();
        return;
    }

    log_message("INITIALIZATION: OBS is ready, proceeding with banner initialization");

    // Clear cleanup flag to allow banner operations
    m_cleanup_in_progress.store(false);
    log_message("INITIALIZATION: Cleanup flag cleared - banner operations enabled");

    // Set flag to prevent duplicate calls during this initialization
    m_initialization_started.store(true);
    
    log_message("INITIALIZATION: User type: " + PremiumStatusHandler::get_user_type_string(this));
    
    if (!PremiumStatusHandler::is_premium(this)) {
        // FREE USERS: Initialize banners after OBS is fully loaded
        log_message("INITIALIZATION: FREE USER - Starting automatic banner initialization");

        // Create single shared banner source
        log_message("INITIALIZATION: Creating single shared banner source");
        create_banner_source();

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
    // NEW ARCHITECTURE: Banner sources are self-managing for visibility/position
    // Banner_manager ONLY handles restoration when banners are removed (for free users)

    // CRITICAL: Prevent multiple signal connections
    if (m_signals_connected.load()) {
        log_message("Signal connections already established - ignoring duplicate call");
        return;
    }

    // Only connect for free users (premium users can remove banners freely)
    if (PremiumStatusHandler::is_premium(this)) {
        log_message("SIGNAL CONNECTIONS: Premium user - skipping (no enforcement needed)");
        return;
    }

    log_message("SIGNAL CONNECTIONS: NO SIGNALS - Using polling timer instead (prevents CEF crash)");

    // DO NOT connect ANY signals - they ALL cause CEF shutdown crashes
    // Scene signals, global signals, source signals - ALL cause the same CEF crash
    // The only solution is polling-based monitoring with a timer

    // Start polling timer to check banner existence every 3 seconds
    // CRITICAL: Store thread (don't detach) so we can join it during shutdown
    m_polling_thread = std::jthread([this](std::stop_token stop_token) {
        log_message("POLLING: Banner restoration timer started (checks every 3 seconds)");

        while (!stop_token.stop_requested() && !m_shutting_down.load()) {
            // Use interruptible sleep - wakes up immediately on stop request
            if (std::this_thread::sleep_for(std::chrono::seconds(3)), stop_token.stop_requested()) {
                break;
            }

            // Double-check shutdown flags after sleep
            if (m_shutting_down.load() || stop_token.stop_requested()) break;

            // Check if banner source exists
            if (!m_banner_source) {
                log_message("POLLING: Banner source NULL - recreating");
                create_banner_source();
                initialize_banners_all_scenes();
                continue;
            }

            // Check if banner is in current scene
            obs_source_t* current_scene_source = obs_frontend_get_current_scene();
            if (current_scene_source) {
                obs_scene_t* scene = obs_scene_from_source(current_scene_source);
                if (scene) {
                    obs_sceneitem_t* item = obs_scene_find_source(scene, m_banner_source_name.c_str());
                    if (!item) {
                        log_message("POLLING: Banner missing from current scene - re-adding");
                        obs_sceneitem_t* new_item = obs_scene_add(scene, m_banner_source);
                        if (new_item) {
                            obs_sceneitem_set_visible(new_item, true);
                            obs_sceneitem_set_locked(new_item, true);
                            obs_sceneitem_set_order(new_item, OBS_ORDER_MOVE_TOP);
                        }
                    }
                }
                obs_source_release(current_scene_source);
            }
        }
        log_message("POLLING: Banner restoration timer stopped (shutdown/stop requested)");
    });

    m_signals_connected.store(true);
    log_message("SIGNAL CONNECTIONS: Polling timer started - NO signal connections = NO CEF crash");
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
            const char* scene_name = obs_source_get_name(scene_source);
            log_message("DEBUG: Connecting signals for scene: " + std::string(scene_name ? scene_name : "unknown"));
            
            // Connect all signal handlers
            signal_handler_connect(handler, "item_add", on_item_add, this);
            signal_handler_connect(handler, "item_remove", on_item_remove, this);
            signal_handler_connect(handler, "item_visible", on_item_visible, this);
            signal_handler_connect(handler, "item_transform", on_item_transform, this);
            signal_handler_connect(handler, "reorder", on_scene_reorder, this);
            
            log_message("DEBUG: Scene signals connected for: " + std::string(scene_name ? scene_name : "unknown"));
            connected_scenes++;
        } else {
            const char* scene_name = obs_source_get_name(scene_source);
            log_message("DEBUG: No signal handler found for scene: " + std::string(scene_name ? scene_name : "unknown"));
        }
    }
    
    obs_frontend_source_list_free(&scenes);
    log_message("Scene signals connected for " + std::to_string(connected_scenes) + " scenes - banner enforcement ACTIVE");
    
    // For free users, ensure all banners are locked and positioned correctly
    if (!m_is_premium.load()) {
        log_message("DEBUG: Free user detected - enforcing banner locks");
        enforce_banner_lock_and_position();
    } else {
        log_message("DEBUG: Premium user - skipping banner locks");
    }
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
            signal_handler_disconnect(handler, "item_add", on_item_add, this);
            signal_handler_disconnect(handler, "item_remove", on_item_remove, this);
            signal_handler_disconnect(handler, "item_visible", on_item_visible, this);
            signal_handler_disconnect(handler, "item_transform", on_item_transform, this);
            signal_handler_disconnect(handler, "reorder", on_scene_reorder, this);
            
            // Standard signal cleanup
                disconnected_scenes++;
        }
    }
    
    obs_frontend_source_list_free(&scenes);
        log_message("Scene signals disconnected from " + std::to_string(disconnected_scenes) + " scenes");
        
        // No complex signal/timer cleanup needed with prevention approach
        
        // Reset flag after disconnection
        m_signals_connected.store(false);
        
    } catch (...) {
        log_message("ERROR: Exception during signal disconnection - ignoring");
        m_signals_connected.store(false);  // Reset flag even if exception
    }
}

// Signal handler for when items are added to scenes
void banner_manager::on_item_add(void* data, calldata_t* calldata)
{
    if (!data || !calldata) return;
    
    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;
    
    try {
        obs_sceneitem_t* item = (obs_sceneitem_t*)calldata_ptr(calldata, "item");
        if (!item) return;
        
        obs_source_t* source = obs_sceneitem_get_source(item);
        if (!source) return;
        
        const char* name = obs_source_get_name(source);
        if (!name) return;
        
        // If any new item is added, ensure our banner stays on top (for free users)
        // Also check for any item changes that might affect order
        if (!manager->m_is_premium.load()) {
            obs_scene_t* scene = obs_sceneitem_get_scene(item);
            if (scene) {
                obs_sceneitem_t* banner_item = manager->find_vortideck_ads_in_scene(scene);
                if (banner_item) {
                    // Check if banner is at the top
                    obs_sceneitem_t* first_item = nullptr;
                    obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t* item, void* param) {
                        obs_sceneitem_t** first_ptr = static_cast<obs_sceneitem_t**>(param);
                        if (!*first_ptr) {
                            *first_ptr = item;
                        }
                        return false; // Stop after first item
                    }, &first_item);
                    
                    if (first_item != banner_item) {
                        manager->log_message("FREE USER: Item change detected - banner not on top, fixing");
                        
                        // Set flag to prevent infinite loop
                        manager->m_correcting_position.store(true);
                        obs_sceneitem_set_order(banner_item, OBS_ORDER_MOVE_TOP);
                        manager->m_correcting_position.store(false);
                        
                        manager->log_message("FREE USER: Banner moved to top via item_add handler");
                    }
                }
            }
        }
        
    } catch (...) {
        manager->log_message("SIGNAL: Exception in item_add handler");
    }
}

// NEW: Global signal handler for source removal (prevents CEF crash)
void banner_manager::on_global_source_remove(void* data, calldata_t* calldata)
{
    if (!data || !calldata) return;

    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;

    // CRITICAL: Check shutdown flags FIRST
    if (manager->m_shutting_down.load() || manager->m_cleanup_in_progress.load()) {
        return;
    }

    // Get the removed source
    obs_source_t* removed_source = (obs_source_t*)calldata_ptr(calldata, "source");
    if (!removed_source) return;

    const char* source_name = obs_source_get_name(removed_source);
    if (!source_name) return;

    // Check if this is our banner source being removed
    if (std::string(source_name) == manager->m_banner_source_name) {
        manager->log_message("GLOBAL SIGNAL: Banner source removed - Name: " + std::string(source_name));

        if (!PremiumStatusHandler::is_premium(manager)) {
            manager->log_message("GLOBAL SIGNAL: FREE USER - Recreating banner source immediately");

            // Recreate the banner source
            manager->create_banner_source();

            // Re-add to all scenes
            manager->initialize_banners_all_scenes();
        }
    }
}

void banner_manager::on_item_remove(void* data, calldata_t* calldata)
{
    if (!data || !calldata) return;

    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;

    // CRITICAL: Check ALL abort conditions FIRST, before touching ANY OBS API
    // This prevents signal handlers from running during shutdown/cleanup
    if (manager->m_shutting_down.load()) {
        return;  // Shutdown in progress - abort immediately
    }

    if (manager->m_cleanup_in_progress.load()) {
        return;  // Cleanup in progress - abort immediately
    }

    if (manager->m_intentional_hide_in_progress.load()) {
        return;  // Intentional hide in progress - abort immediately
    }

    try {
        obs_sceneitem_t* item = (obs_sceneitem_t*)calldata_ptr(calldata, "item");
        if (!item) return;

        obs_source_t* source = obs_sceneitem_get_source(item);
        if (!source) return;

        const char* name = obs_source_get_name(source);
        if (!name) return;

        // Check if this is our ADS being removed (metadata-based detection)
        if (manager->is_vortideck_ads_item(item)) {
            manager->log_message("SIGNAL: Banner removal detected - Name: " + std::string(name) +
                               ", User: " + PremiumStatusHandler::get_user_type_string(manager));

            // SYNCHRONOUS restoration - no delayed threads, happens immediately
            // This prevents CEF shutdown crashes by avoiding late banner creation

            if (!PremiumStatusHandler::is_premium(manager)) {
                manager->log_message("SIGNAL: FREE USER - Banner restoration SYNCHRONOUS (immediate)");

                // All critical checks already done at top of function
                // Safe to proceed with restoration

                // Check if intentional hide is in progress
                if (!manager->m_intentional_hide_in_progress.load()) {
                    manager->log_message("SIGNAL: Calling enforce_banner_visibility SYNCHRONOUSLY");
                    manager->enforce_banner_visibility();
                    manager->cleanup_banner_names_after_deletion();
                } else {
                    manager->log_message("SIGNAL: Skipping restoration - intentional hide in progress");
                }
            } else {
                // PREMIUM USER: Trigger cleanup after banner deletion
                PremiumStatusHandler::log_premium_action(manager, "banner deletion", "triggering cleanup");

                // Trigger cleanup in a jthread to avoid blocking the signal
                std::jthread([manager]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));

                    // CRITICAL: Check shutdown flag before touching OBS API
                    if (manager->m_shutting_down.load()) {
                        manager->log_message("SIGNAL: Shutdown detected, aborting cleanup");
                        return;
                    }

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

    // CRITICAL: Check ALL abort conditions FIRST, before touching ANY OBS API
    if (manager->m_shutting_down.load()) {
        return;  // Shutdown in progress - abort immediately
    }

    if (manager->m_cleanup_in_progress.load()) {
        return;  // Cleanup in progress - abort immediately
    }

    if (manager->m_intentional_hide_in_progress.load()) {
        return;  // Intentional hide in progress - abort immediately
    }

    try {
        obs_sceneitem_t* item = (obs_sceneitem_t*)calldata_ptr(calldata, "item");
        if (!item) return;
    
        bool visible = calldata_bool(calldata, "visible");
    
        obs_source_t* source = obs_sceneitem_get_source(item);
        if (!source) return;
    
        const char* name = obs_source_get_name(source);
        if (!name) return;
        
        // Check if this is our ADS visibility change (metadata-based detection)
        if (manager->is_vortideck_ads_item(item)) {
            bool is_premium = manager->m_is_premium.load();
            
            // Only log significant changes to reduce spam
            if (!visible) {
                manager->log_message("SIGNAL: Banner hidden detected - Name: " + std::string(name) + 
                                   ", User: " + (is_premium ? "premium" : "free"));
            }
            
            if (!is_premium && !visible) {
                // DISABLED: Automatic visibility restoration causes CEF shutdown issues
                manager->log_message("SIGNAL: FREE USER - Visibility restoration DISABLED to prevent CEF crashes");
                return;

                manager->log_message("SIGNAL: FREE USER - Immediate banner restoration (jthread-based)");

                // jthread-based restoration for free users - more reliable than direct call
                std::jthread([manager, item, name]() {
                    try {
                        // Small delay to ensure the visibility change is complete
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));

                        // CRITICAL: Check shutdown flag before touching OBS API
                        if (manager->m_shutting_down.load()) {
                            manager->log_message("SIGNAL: Shutdown detected, aborting visibility restoration");
                            return;
                        }

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

    // CRITICAL: Check ALL abort conditions FIRST, before touching ANY OBS API
    if (manager->m_shutting_down.load()) {
        return;  // Shutdown in progress - abort immediately
    }

    if (manager->m_cleanup_in_progress.load()) {
        return;  // Cleanup in progress - abort immediately
    }
    
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
    
        // Check if this is our ADS being moved/resized (metadata-based detection)
        if (manager->is_vortideck_ads_item(item)) {
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

// Prevention-based banner enforcement - lock banners so they can't be moved
void banner_manager::enforce_banner_lock_and_position()
{
    log_message("PREVENTION: Enforcing banner lock and position for free users");
    
    try {
        // Get all scenes and ensure banners are locked at top position
        obs_frontend_source_list scenes = {};
        obs_frontend_get_scenes(&scenes);
        
        for (size_t i = 0; i < scenes.sources.num; i++) {
            obs_source_t* scene_source = scenes.sources.array[i];
            if (!scene_source) continue;
            
            obs_scene_t* scene = obs_scene_from_source(scene_source);
            if (!scene) continue;
            
            obs_sceneitem_t* banner_item = find_vortideck_ads_in_scene(scene);
            if (banner_item) {
                const char* scene_name = obs_source_get_name(scene_source);
                log_message("PREVENTION: Found banner in scene: " + std::string(scene_name ? scene_name : "unknown"));
                
                // Move to top position first
                obs_sceneitem_set_order(banner_item, OBS_ORDER_MOVE_TOP);
                
                // Lock the banner so it can't be manually moved
                obs_sceneitem_set_locked(banner_item, true);
                
                log_message("PREVENTION: Banner locked at top position");
            }
        }
        
        obs_frontend_source_list_free(&scenes);
        log_message("PREVENTION: Banner enforcement complete - banners are now unmovable");
        
    } catch (...) {
        log_message("PREVENTION: Exception during banner lock enforcement");
    }
}

// No timer code needed - prevention approach is cleaner

// Source signal handlers for direct visibility monitoring
void banner_manager::on_source_hide(void* data, calldata_t* calldata)
{
    if (!data) return;

    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;

    try {
        // CRITICAL: Check shutdown FIRST - don't process signals during shutdown
        if (manager->m_shutting_down.load()) {
            manager->log_message("SOURCE SIGNAL: Ignoring hide signal during shutdown");
            return;
        }

        manager->m_source_visible.store(false);
        manager->log_message("SOURCE SIGNAL: Banner source hidden");

        // For free users, immediately enforce visibility
        if (!manager->m_is_premium.load() && !manager->m_intentional_hide_in_progress.load()) {
            manager->log_message("FREE USER: Banner hidden via source - enforcing visibility");
            manager->enforce_banner_visibility();
        }
    } catch (...) {
        manager->log_message("SIGNAL: Exception in source_hide handler");
    }
}

void banner_manager::on_source_show(void* data, calldata_t* calldata)
{
    if (!data) return;
    
    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;
    
    try {
        manager->m_source_visible.store(true);
        manager->log_message("SOURCE SIGNAL: Banner source shown");
    } catch (...) {
        manager->log_message("SIGNAL: Exception in source_show handler");
    }
}

void banner_manager::on_source_deactivate(void* data, calldata_t* calldata)
{
    if (!data) return;
    
    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;
    
    try {
        manager->log_message("SOURCE SIGNAL: Banner deactivated from stream/recording");
        
        // For free users during streaming, enforce visibility
        if (!manager->m_is_premium.load() && obs_frontend_streaming_active()) {
            manager->log_message("FREE USER: Banner deactivated during stream - enforcing visibility");
            manager->enforce_banner_visibility();
        }
    } catch (...) {
        manager->log_message("SIGNAL: Exception in source_deactivate handler");
    }
}

void banner_manager::on_source_activate(void* data, calldata_t* calldata)
{
    if (!data) return;
    
    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;
    
    try {
        manager->log_message("SOURCE SIGNAL: Banner activated on stream/recording");
    } catch (...) {
        manager->log_message("SIGNAL: Exception in source_activate handler");
    }
}

// CRITICAL: Scene reorder signal handler - defer banner enforcement to avoid timing issues
void banner_manager::on_scene_reorder(void* data, calldata_t* calldata)
{
    if (!data) return;
    
    banner_manager* manager = static_cast<banner_manager*>(data);
    if (!manager) return;
    
    // Only for free users - premium users can reorder freely
    if (manager->m_is_premium.load()) {
        return;
    }
    
    // Prevent infinite loops during our correction
    if (manager->m_correcting_position.load()) {
        return;
    }
    
    try {
        manager->log_message("REORDER SIGNAL: Scene items reordered - deferring banner enforcement");
        
        // Get the scene from calldata
        obs_scene_t* scene = nullptr;
        if (calldata_get_ptr(calldata, "scene", (void**)&scene) && scene) {
            // Defer the correction by 100ms to let the reorder operation complete
            std::thread([manager, scene]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                // CRITICAL: Check shutdown flag before touching OBS API
                if (manager->m_shutting_down.load()) {
                    manager->log_message("REORDER: Shutdown detected, aborting reorder correction");
                    return;
                }

                obs_sceneitem_t* banner_item = manager->find_vortideck_ads_in_scene(scene);
                if (banner_item) {
                    manager->m_correcting_position.store(true);

                    // Log current position before correction
                    obs_source_t* scene_source = obs_scene_get_source(scene);
                    const char* scene_name = obs_source_get_name(scene_source);
                    manager->log_message("REORDER: Banner found in scene: " + std::string(scene_name ? scene_name : "unknown"));

                    // Try different reorder approaches
                    obs_sceneitem_set_order(banner_item, OBS_ORDER_MOVE_TOP);
                    manager->log_message("REORDER: OBS_ORDER_MOVE_TOP called");

                    // Check if banner is actually at the top now by enumerating scene items
                    bool banner_is_first = false;
                    obs_scene_enum_items(scene, [](obs_scene_t* scene, obs_sceneitem_t* item, void* param) {
                        bool* is_first = static_cast<bool*>(param);
                        obs_source_t* source = obs_sceneitem_get_source(item);
                        const char* source_id = obs_source_get_id(source);
                        if (source_id && strcmp(source_id, "vortideck_ads") == 0) {
                            *is_first = true;
                        }
                        return false; // Stop after first item (which should be at top)
                    }, &banner_is_first);

                    manager->log_message("REORDER: Banner is now at top: " + std::string(banner_is_first ? "YES" : "NO"));

                    manager->m_correcting_position.store(false);
                }
            }).detach();
        }
    } catch (...) {
        manager->log_message("REORDER SIGNAL: Exception in scene reorder handler");
        manager->m_correcting_position.store(false);
    }
}

void banner_manager::connect_source_signals()
{
    // NOTE: With per-scene wrappers, we don't need per-source signal monitoring
    // Each wrapper manages its own signals internally
    log_message("SOURCE SIGNALS: Not needed with per-scene wrapper architecture");
}

void banner_manager::disconnect_source_signals()
{
    // NOTE: With per-scene wrappers, we don't need per-source signal monitoring
    // Each wrapper manages its own signals internally
    log_message("SOURCE SIGNALS: Not needed with per-scene wrapper architecture");
}

void banner_manager::cleanup_for_scene_collection_change()
{
    log_message("Scene collection changing - cleaning up banner sources");

    // CRITICAL: Set cleanup flag to prevent signal handlers during source destruction
    // Per web research: signal_handler_disconnect() doesn't prevent already-queued signals
    // So we MUST use flags to abort signal handlers
    m_cleanup_in_progress.store(true);
    log_message("CLEANUP: Cleanup flag set - signal handlers will abort during source destruction");

    // Stop monitoring
    stop_persistence_monitor();

    // Disconnect signals (queued signals will still fire but cleanup flag will stop them)
    disconnect_scene_signals();
    disconnect_source_signals();

    // CRITICAL: Do NOT remove ANY sources manually - this triggers item_remove signals
    // Even with signals disconnected, queued signals fire after disconnect returns
    // Let OBS handle all source destruction naturally
    log_message("CLEANUP: NOT removing banners manually - OBS will handle destruction");

    // Clear visibility
    m_banner_visible = false;

    // Cleanup flag will be cleared by initialize_after_obs_ready() when collection loads
    // If OBS is shutting down, initialize will never be called and flag stays set
    log_message("CLEANUP: Banner cleanup complete - waiting for collection load or shutdown");
}

void banner_manager::show_banner(bool enable_duration_timer)
{
    // CRITICAL: Check shutdown flag FIRST before touching OBS API
    if (m_shutting_down.load()) {
        log_message("SHOW_BANNER: Shutdown detected, aborting banner display");
        return;
    }

    log_message("SHOW_BANNER: Starting banner display process...");

    log_message("SHOW_BANNER: User type: " + PremiumStatusHandler::get_user_type_string(this));

    // AD FREQUENCY LIMITS REMOVED - handled by external application
    // Premium users can show banners anytime, free users controlled externally

    // Window management removed - handled by external application

    // NOTE: No need to check m_banner_source - wrappers are created on-demand per-scene
    log_message("SHOW_BANNER: Per-scene wrappers will be created on-demand");

    if (!PremiumStatusHandler::is_premium(this)) {
        // FREE USERS: FORCED banner initialization across ALL scenes
        PremiumStatusHandler::log_premium_action(this, "banner display", "forcing across all scenes");
        initialize_banners_all_scenes();
        m_banner_visible = true;
        
        log_message("SHOW_BANNER: FREE USER - Banner display complete with restrictions");
        log_message("SHOW_BANNER: FREE USER - Limited hiding (5sec auto-restore), no positioning control");
        
        // For free users, signals will handle visibility monitoring
        log_message("SHOW_BANNER: FREE USER - Using signal-based protection (no polling)");

        // NOTE: Source signals not needed with per-scene wrappers

        // FORCE immediate enforcement for FREE USERS
        log_message("SHOW_BANNER: FREE USER - Enforcing banner visibility");
        enforce_banner_visibility();

        // CRITICAL: Also enforce banner lock and position
        log_message("SHOW_BANNER: FREE USER - Enforcing banner lock and position");
        enforce_banner_lock_and_position();

        // Duration control removed - handled by external application
        log_message("SHOW_BANNER: FREE USER - Duration managed externally");
    } else {
        // PREMIUM USERS: Complete freedom - NO forced banner initialization
        PremiumStatusHandler::log_premium_action(this, "banner display", "complete freedom mode (no forced banners)");
        log_message("SHOW_BANNER: PREMIUM USER - Use WebSocket API to add banners if desired");
        // NO automatic banner creation for premium users
        // They have complete choice whether to have banners or not

        // Duration control removed - handled by external application
        if (m_banner_visible) {
            log_message("SHOW_BANNER: PREMIUM USER - Duration managed externally");
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
        
        // FREE USERS: No delayed timers - restoration happens SYNCHRONOUSLY via signals
        // Clear intentional hide flag immediately so signals can restore banners
        m_intentional_hide_in_progress.store(false);
        log_message("HIDE_BANNER: FREE USER - Banner will be restored SYNCHRONOUSLY by signal handlers (no delays)");
        
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

void banner_manager::set_banner_url(const std::string& url)
{
    log_message("BANNER URL: set_banner_url() called with URL: " + url);

    // CRITICAL FIX: This function used to create direct browser sources (line 1085)
    // which were orphaned and caused CEF shutdown crashes.
    //
    // NOW: This function does NOTHING except log. The URL parameter is passed directly
    // to create_banner_source() by the caller. This function exists only for API compatibility.
    //
    // All source creation now happens through create_banner_source() which properly creates
    // vortideck_banner_menu wrappers with active child registration for clean CEF shutdown.

    log_message("BANNER URL: No action taken - banner creation handled by create_banner_source()");
}

void banner_manager::resize_banner_to_canvas()
{
    log_message("BANNER RESIZE: Resizing banner to match current canvas size");

    // NOTE: With per-scene wrappers, each wrapper handles its own sizing
    // Banner sources are automatically sized to canvas
    log_message("BANNER RESIZE: Per-scene wrappers handle sizing automatically");

    // Force show banner to ensure it's visible after resize
    show_banner();

    log_message("BANNER RESIZE: Banner successfully resized and repositioned");
}

void banner_manager::show_premium_banner()
{
    // PREMIUM USERS ONLY: Add banner to current scene by choice
    if (!PremiumStatusHandler::handle_premium_restriction(this, "banner_position", "premium banner function")) {
        log_message("FREE USER: Cannot use premium banner function - banners are automatically managed");
        return;
    }

    // NOTE: With per-scene wrappers, wrappers are created on-demand
    log_message("PREMIUM USER: Adding banner to current scene with per-scene wrapper");

    // Add to current scene only (premium user's choice)
    add_banner_to_current_scene();
    m_banner_visible = true;

    PremiumStatusHandler::log_premium_action(this, "banner added to current scene", "you have full control");
}

bool banner_manager::is_banner_visible() const
{
    // NOTE: With per-scene wrappers, check if any banners exist in any scene
    // PERFORMANCE IMPROVEMENT: Use cached source visibility from signals
    // This eliminates the need to iterate through all scenes every time
    if (m_source_visible.load()) {
        return true;
    }
    
    // Fallback: Check scene items for precise visibility (rare case)
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    
    bool found_visible = false;
    
    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* source = scenes.sources.array[i];
        if (!source) continue;
        
        obs_scene_t* scene = obs_scene_from_source(source);
        if (!scene) continue;
        
        // Use metadata-based detection instead of name-based
        obs_sceneitem_t* item = find_vortideck_ads_in_scene(scene);
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
    // NOTE: With per-scene wrappers and no stored references, find by name pattern
    // Return any wrapper source if it exists
    obs_source_t* found = nullptr;

    obs_enum_sources([](void* param, obs_source_t* source) {
        obs_source_t** found_ptr = static_cast<obs_source_t**>(param);
        const char* source_id = obs_source_get_id(source);

        // Find first vortideck_banner_menu wrapper
        if (source_id && strcmp(source_id, "vortideck_banner_menu") == 0) {
            *found_ptr = source;
            return false;  // Stop enumeration
        }

        return true;  // Continue enumeration
    }, &found);

    return found;
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
        if (manager->is_banner_visible()) {
            manager->log_message("FREE USER: Banners already initialized - menu action ignored to prevent duplicates");
            return;
        }
        
        // Check if content is set - if not, create a transparent placeholder
        if (manager->get_current_banner_content().empty()) {
            manager->log_message("FREE USER: Creating banner with connected service URL (menu action)");
            manager->create_banner_source();
        }
        
        // Show the banner - FREE USERS get FORCED banners in ALL scenes
        manager->show_banner();
        
        manager->log_message("FREE USER: Demo banner created via menu - upgrade to premium for banner freedom");
    }
}

void banner_manager::create_banner_source(std::string_view content_data, std::string_view content_type)
{
    // CRITICAL: Check shutdown flag FIRST
    if (m_shutting_down.load()) {
        log_message("BANNER CREATION: Shutdown detected, aborting");
        return;
    }

    log_message("BANNER CREATION: Creating single shared banner source");

    // Build banner URL from connected WebSocket server
    extern std::string get_global_websocket_url();
    std::string websocket_url = get_global_websocket_url();
    std::string banner_url;

    if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
        std::string base_url;
        if (websocket_url.starts_with("ws://")) {
            base_url = "http://" + websocket_url.substr(5);
        } else {
            base_url = "https://" + websocket_url.substr(6);
        }
        if (base_url.ends_with("/ws")) {
            base_url = base_url.substr(0, base_url.length() - 3);
        }
        if (!base_url.ends_with("/")) {
            base_url += "/";
        }
        banner_url = base_url + "banners";
    } else {
        banner_url = websocket_url + "/banners";
    }

    log_message("BANNER CREATION: Banner URL: " + banner_url);

    // Check if banner source already exists
    obs_source_t* existing_source = obs_get_source_by_name(m_banner_source_name.c_str());
    if (existing_source) {
        const char* source_id = obs_source_get_id(existing_source);
        log_message("BANNER CREATION: Found existing banner source with type: " + std::string(source_id ? source_id : "unknown"));

        // Check if it's the OLD browser_source type (needs migration)
        if (source_id && strcmp(source_id, "browser_source") == 0) {
            log_message("BANNER CREATION: OLD browser_source detected - DELETING for migration to vortideck_banner_menu");
            obs_source_remove(existing_source);
            obs_source_release(existing_source);

            // Remove from all scenes
            obs_frontend_source_list scenes = {};
            obs_frontend_get_scenes(&scenes);
            if (scenes.sources.array) {
                for (size_t i = 0; i < scenes.sources.num; i++) {
                    obs_source_t* scene_source = scenes.sources.array[i];
                    if (scene_source) {
                        obs_scene_t* scene = obs_scene_from_source(scene_source);
                        if (scene) {
                            obs_sceneitem_t* item = obs_scene_find_source(scene, m_banner_source_name.c_str());
                            if (item) {
                                obs_sceneitem_remove(item);
                            }
                        }
                    }
                }
                obs_frontend_source_list_free(&scenes);
            }

            log_message("BANNER CREATION: Old browser_source deleted, will create new vortideck_banner_menu wrapper");
            // Continue to create new wrapper below
        } else {
            // It's already a vortideck_banner_menu wrapper, just update URL
            log_message("BANNER CREATION: Found existing vortideck_banner_menu wrapper, updating URL");
            if (m_banner_source) {
                obs_source_release(m_banner_source);
            }
            m_banner_source = existing_source;

            // Update URL
            obs_data_t* settings = obs_data_create();
            obs_data_set_string(settings, "url", banner_url.c_str());
            obs_source_update(m_banner_source, settings);
            obs_data_release(settings);
            return;
        }
    }

    // CRITICAL FIX: Use vortideck_banner_menu wrapper (like overlays use vortideck_overlay)
    // The wrapper creates a browser_source as an active child, preventing CEF/WASAPI crashes
    obs_data_t* settings = obs_data_create();
    obs_data_set_string(settings, "url", banner_url.c_str());
    obs_data_set_string(settings, "banner_id", "main_banner");  // Banner ID for the wrapper

    // Create vortideck_banner_menu wrapper (NOT raw browser_source)
    // The wrapper will internally create the browser_source as an active child
    m_banner_source = obs_source_create("vortideck_banner_menu", m_banner_source_name.c_str(), settings, nullptr);
    obs_data_release(settings);

    if (m_banner_source) {
        log_message("BANNER CREATION: Successfully created vortideck_banner_menu wrapper (prevents CEF crashes)");
    } else {
        log_message("BANNER CREATION: ERROR - Failed to create banner wrapper source!");
    }
}

// ============================================================================
// PER-SCENE WRAPPER MANAGEMENT (NEW - Prevents CEF shutdown crashes)
// ============================================================================

obs_source_t* banner_manager::get_or_create_wrapper_for_scene(const std::string& scene_name)
{
    log_message("WRAPPER: Getting/creating wrapper for scene: " + scene_name);

    // Build unique name for this wrapper
    std::string wrapper_name = m_banner_source_name + " (" + scene_name + ")";

    // Check if wrapper already exists by name
    obs_source_t* existing = obs_get_source_by_name(wrapper_name.c_str());
    if (existing) {
        log_message("WRAPPER: Found existing wrapper for scene: " + scene_name);
        // obs_get_source_by_name increments refcount - return it, caller must release
        return existing;
    }

    // Create new wrapper for this scene
    log_message("WRAPPER: Creating NEW wrapper for scene: " + scene_name);

    // Get WebSocket URL for banner (same for all wrappers)
    extern std::string get_global_websocket_url();
    std::string websocket_url = get_global_websocket_url();
    std::string banner_url;

    if (websocket_url.starts_with("ws://") || websocket_url.starts_with("wss://")) {
        // Convert WebSocket URL to HTTP URL and add /banners
        std::string base_url;
        if (websocket_url.starts_with("ws://")) {
            base_url = "http://" + websocket_url.substr(5);
        } else {
            base_url = "https://" + websocket_url.substr(6);
        }
        // Remove /ws path if present
        if (base_url.ends_with("/ws")) {
            base_url = base_url.substr(0, base_url.length() - 3);
        }
        // Ensure no double slashes
        if (base_url.ends_with("/")) {
            banner_url = base_url + "banners";
        } else {
            banner_url = base_url + "/banners";
        }
    } else {
        banner_url = websocket_url + "/banners";
    }

    // Create settings for wrapper
    obs_data_t* settings = obs_data_create();
    obs_data_set_string(settings, "url", banner_url.c_str());

    // Create wrapper source (vortideck_banner_menu creates browser as active child)
    obs_source_t* wrapper = obs_source_create("vortideck_banner_menu", wrapper_name.c_str(), settings, nullptr);
    obs_data_release(settings);

    if (wrapper) {
        log_message("WRAPPER: SUCCESS - Created wrapper for scene: " + scene_name + " with URL: " + banner_url);

        // IMPORTANT: This function returns a strong reference (refcount +1)
        // Caller MUST call obs_source_release() after adding to scene
        // obs_scene_add() adds its own reference, so caller's reference must be released
        // This ensures proper lifecycle: scene holds reference, manager doesn't

        return wrapper;
    } else {
        log_message("WRAPPER: FAILED - Could not create wrapper for scene: " + scene_name);
        return nullptr;
    }
}

void banner_manager::release_wrapper_for_scene(const std::string& scene_name)
{
    // NOTE: We don't hold references anymore - OBS manages wrapper lifecycle
    // This method is kept for API compatibility but does nothing
    log_message("WRAPPER: Not holding references - OBS will clean up wrapper for scene: " + scene_name);
}

void banner_manager::release_all_wrappers()
{
    // NOTE: We don't hold references anymore - OBS manages wrapper lifecycle
    // This allows OBS to properly clean up sources during shutdown
    log_message("WRAPPER: Not holding references - OBS will clean up all wrappers automatically");
}

// void banner_manager::create_banner_source_with_custom_params(std::string_view content_data, std::string_view content_type,
//                                                              std::string_view custom_css, int custom_width, int custom_height, bool css_locked)
// {
//     log_message(std::format("ENHANCED BANNER CREATION: Starting banner source creation with custom params - Type: {}, Data: {}...", 
//                           content_type, content_data.substr(0, 50)));
//     log_message(std::format("ENHANCED BANNER CREATION: Custom CSS: {}, Dimensions: {}x{}", 
//                           (custom_css.empty() ? "none" : "provided"), custom_width, custom_height));
    
//     // Convert content to URL format for browser source (needed for both reuse and create)
//     std::string url_content;
//     std::string css_content = "";
    
//     if (content_type == "transparent") {
//         // Create completely transparent banner (no visible content)
//         url_content = "data:text/html,<html><body></body></html>";
//         css_content = custom_css.empty() ? 
//             "body { margin: 0; padding: 0; background: transparent; width: 100vw; height: 100vh; overflow: hidden; }" :
//             std::string(custom_css);
//     } else if (content_type == "color") {
//         // Create HTML page with color background
//         std::string color = "#FF0000"; // Default red
//         if (content_data.length() >= 7 && content_data[0] == '#') {
//             color = std::string(content_data);
//         }
//         url_content = "data:text/html,<html><body><h2>VortiDeck Banner</h2></body></html>";
//         css_content = custom_css.empty() ? 
//             std::format("body {{ background-color: {}; margin: 0; padding: 20px; font-family: Arial; color: white; text-align: center; display: flex; align-items: center; justify-content: center; }}", color) :
//             std::string(custom_css);
//     } else if (content_type == "text") {
//         // Create HTML page with text content
//         url_content = std::format("data:text/html,<html><body>{}</body></html>", content_data);
//         css_content = custom_css.empty() ? 
//             "body { background-color: #000; margin: 0; padding: 20px; font-family: Arial, sans-serif; color: white; text-align: center; font-size: 24px; display: flex; align-items: center; justify-content: center; }" :
//             std::string(custom_css);
//                     } else if (is_image_content(content_type) || is_video_content(content_type)) {
//         // For image/video URLs - check if custom CSS needs HTML structure
//         if (is_url(content_data)) {
//             // Check if custom CSS references HTML elements (img, video, etc.)
//             bool needs_html_wrapper = !custom_css.empty() && 
//                                     (custom_css.find("img") != std::string::npos || 
//                                      custom_css.find("video") != std::string::npos ||
//                                      custom_css.find("@keyframes") != std::string::npos ||
//                                      custom_css.find("animation") != std::string::npos);
            
//             if (needs_html_wrapper) {
//                 // Create HTML wrapper with image/video element for CSS targeting
//                 std::string element_tag = is_video_content(content_type) ? "video" : "img";
//                 std::string extra_attrs = is_video_content(content_type) ? " autoplay loop muted" : "";
                
//                 // FLEXIBLE CSS HANDLING: Use explicit css_locked parameter with smart defaults
//                 // If css_locked is not explicitly set, use smart defaults:
//                 // - Premium users: CSS editable by default 
//                 // - Free users: CSS locked by default (for ad content protection)
//                 bool final_css_locked = css_locked;
//                 if (!css_locked && !PremiumStatusHandler::is_premium(this)) {
//                     // Free users get locked CSS by default unless explicitly overridden
//                     final_css_locked = true;
//                     log_message("BANNER CREATION: Auto-locking CSS for free user (set css_locked=false to override)");
//                 }
                
//                 if (final_css_locked) {
//                     // LOCKED MODE: Embed CSS in HTML (ads, protected content)
//                     url_content = std::format("data:text/html,<html><head><style>{}</style></head><body><{} src=\"{}\"{}></{}</body></html>", 
//                                             custom_css, element_tag, content_data, extra_attrs, element_tag);
//                     css_content = ""; // CSS is hidden in HTML
//                     log_message("BANNER CREATION: CSS LOCKED - Embedded in HTML (not visible in OBS properties)");
//                 } else {
//                     // EDITABLE MODE: CSS visible in OBS browser source properties
//                     // Create basic HTML structure without embedded CSS
//                     url_content = std::format("data:text/html,<html><body><{} src=\"{}\"{}></{}</body></html>", 
//                                             element_tag, content_data, extra_attrs, element_tag);
//                     css_content = std::string(custom_css); // CSS will be visible in OBS
//                     log_message("BANNER CREATION: CSS EDITABLE - Visible in OBS properties for inspection/modification");
//                 }
                
//                 log_message(std::format("BANNER CREATION: Created HTML wrapper for image/video with custom CSS - Mode: {}", 
//                                        final_css_locked ? "LOCKED" : "EDITABLE"));
//             } else {
//                 // Direct image/video URL (no custom CSS or CSS doesn't need HTML structure)
//                 url_content = std::string(content_data);
//                 css_content = custom_css.empty() ? 
//                     "body { margin: 0; padding: 0; background: transparent; overflow: hidden; }" :
//                     std::string(custom_css);
//             }
//         } else {
//             // Local file or other image URL - same logic
//             bool needs_html_wrapper = !custom_css.empty() && 
//                                     (custom_css.find("img") != std::string::npos || 
//                                      custom_css.find("video") != std::string::npos ||
//                                      custom_css.find("@keyframes") != std::string::npos ||
//                                      custom_css.find("animation") != std::string::npos);
            
//             if (needs_html_wrapper) {
//                 std::string element_tag = is_video_content(content_type) ? "video" : "img";
//                 std::string extra_attrs = is_video_content(content_type) ? " autoplay loop muted" : "";
                
//                 // FLEXIBLE CSS HANDLING: Use explicit css_locked parameter with smart defaults
//                 // If css_locked is not explicitly set, use smart defaults:
//                 // - Premium users: CSS editable by default 
//                 // - Free users: CSS locked by default (for ad content protection)
//                 bool final_css_locked = css_locked;
//                 if (!css_locked && !PremiumStatusHandler::is_premium(this)) {
//                     // Free users get locked CSS by default unless explicitly overridden
//                     final_css_locked = true;
//                     log_message("BANNER CREATION: Auto-locking CSS for free user (set css_locked=false to override)");
//                 }
                
//                 if (final_css_locked) {
//                     // LOCKED MODE: Embed CSS in HTML (ads, protected content)
//                     url_content = std::format("data:text/html,<html><head><style>{}</style></head><body><{} src=\"{}\"{}></{}>></body></html>", 
//                                             custom_css, element_tag, content_data, extra_attrs, element_tag);
//                     css_content = ""; // CSS is hidden in HTML
//                     log_message("BANNER CREATION: CSS LOCKED - Embedded in HTML (not visible in OBS properties)");
//                 } else {
//                     // EDITABLE MODE: CSS visible in OBS browser source properties
//                     // Create basic HTML structure without embedded CSS
//                     url_content = std::format("data:text/html,<html><body><{} src=\"{}\"{}></{}>></body></html>", 
//                                             element_tag, content_data, extra_attrs, element_tag);
//                     css_content = std::string(custom_css); // CSS will be visible in OBS
//                     log_message("BANNER CREATION: CSS EDITABLE - Visible in OBS properties for inspection/modification");
//                 }
                
//                 log_message(std::format("BANNER CREATION: Local file HTML wrapper with custom CSS - Mode: {}", 
//                            final_css_locked ? "LOCKED" : "EDITABLE"));
//             } else {
//                 url_content = std::string(content_data);
//                 css_content = custom_css.empty() ? 
//                     "body { margin: 0; padding: 0; background: transparent; overflow: hidden; }" :
//                     std::string(custom_css);
//             }
//         }
//     } else if (is_url(content_data)) {
//         // Direct URL - use as is
//         url_content = std::string(content_data);
//         css_content = std::string(custom_css); // Use provided CSS or empty
//     } else {
//         // Default fallback - treat as text
//         url_content = std::format("data:text/html,<html><body>{}</body></html>", content_data);
//         css_content = custom_css.empty() ? 
//             "body { background-color: #000; margin: 0; padding: 20px; font-family: Arial, sans-serif; color: white; text-align: center; font-size: 24px; display: flex; align-items: center; justify-content: center; }" :
//             std::string(custom_css);
//     }

//     obs_source_t* existing_source = obs_get_source_by_name(m_banner_source_name.c_str());
//     if (existing_source) {
//         // Check if existing source is actually a browser source
//         const char* source_id = obs_source_get_id(existing_source);
//         if (source_id && std::string(source_id) == "browser_source") {
//             log_message("ENHANCED BANNER CREATION: Found existing browser source, reusing it");
            
//             // Release our current source if any
//             if (m_banner_source) {
//                 obs_source_release(m_banner_source);
//             }
            
//             // Use the existing source
//             m_banner_source = existing_source;
            
//             // Update the existing browser source with new URL content (use the shared conversion logic)
//             obs_data_t* settings = obs_data_create();
            
//             // Set browser source properties with custom dimensions
//             obs_data_set_string(settings, "url", url_content.c_str());
            
//             // FULL-SCREEN BANNERS: Use custom dimensions if provided, otherwise use full canvas size
//             // Get OBS canvas resolution for full-screen banners
//             obs_video_info ovi;
//             obs_get_video_info(&ovi);
            
//             int canvas_width = ovi.base_width > 0 ? ovi.base_width : 1920;
//             int canvas_height = ovi.base_height > 0 ? ovi.base_height : 1080;
            
//             // ALWAYS use full canvas size for browser source (ignore custom width/height)
//             int width = canvas_width;
//             int height = canvas_height;
            
//             log_message("FULL-SCREEN BANNER: Browser source dimensions " + std::to_string(width) + "x" + std::to_string(height) + 
//                        " (Full Canvas - Custom w/h " + std::to_string(custom_width) + "x" + std::to_string(custom_height) + " for CSS only)");
            
//             obs_data_set_int(settings, "width", width);
//             obs_data_set_int(settings, "height", height);
            
//             obs_data_set_int(settings, "fps", 30);
//             obs_data_set_bool(settings, "reroute_audio", false);
//             obs_data_set_bool(settings, "restart_when_active", true); // Force refresh on content change
//             obs_data_set_bool(settings, "shutdown", false);
//             obs_data_set_string(settings, "css", css_content.c_str());
            
//             // Update the browser source
//             obs_source_update(m_banner_source, settings);
            
//             // Force a refresh by toggling the source active state AND triggering browser refresh
//             obs_source_set_enabled(m_banner_source, false);
//             obs_source_set_enabled(m_banner_source, true);
            
//             // Force browser source refresh for new content
//             log_message("ENHANCED BANNER CREATION: Forced browser source refresh for new content");
            
//             obs_data_release(settings);
            
//             log_message(std::format("ENHANCED BANNER CREATION: Successfully reused existing browser source - Type: {}", content_type));
//             log_message(std::format("ENHANCED BANNER CREATION: Updated URL: {}{}", url_content.substr(0, 100), (url_content.length() > 100 ? "..." : "")));
//             log_message(std::format("ENHANCED BANNER CREATION: Dimensions: {}x{}", width, height));
//             log_message(std::format("ENHANCED BANNER CREATION: CSS: {}", (custom_css.empty() ? "default" : "custom")));
//             return;
//         } else {
//             // Existing source is NOT a browser source (probably old custom source) - remove it and create new browser source
//             log_message("ENHANCED BANNER CREATION: Found existing NON-browser source (probably old custom), removing it");
//             obs_source_release(existing_source);
            
//             // Remove the old source from all scenes first
//             obs_frontend_source_list scenes = {};
//             obs_frontend_get_scenes(&scenes);
            
//             for (size_t i = 0; i < scenes.sources.num; i++) {
//                 obs_source_t* scene_source = scenes.sources.array[i];
//                 if (!scene_source) continue;
                
//                 obs_scene_t* scene = obs_scene_from_source(scene_source);
//                 if (!scene) continue;
                
//                 obs_sceneitem_t* item = obs_scene_find_source(scene, m_banner_source_name.c_str());
//                 if (item) {
//                     obs_sceneitem_remove(item);
//                     log_message("ENHANCED BANNER CREATION: Removed old source from scene");
//                 }
//             }
//             obs_frontend_source_list_free(&scenes);
//         }
//     }
    
//     // Create settings for browser_source directly
//     obs_data_t* settings = obs_data_create();
    
//     // Set browser source properties with custom dimensions
//     obs_data_set_string(settings, "url", url_content.c_str());
    
//     // FULL-SCREEN BANNERS: Use custom dimensions if provided, otherwise use full canvas size
//     // Get OBS canvas resolution for full-screen banners
//     obs_video_info ovi;
//     obs_get_video_info(&ovi);
    
//     int canvas_width = ovi.base_width > 0 ? ovi.base_width : 1920;
//     int canvas_height = ovi.base_height > 0 ? ovi.base_height : 1080;
    
//     // ALWAYS use full canvas size for browser source (ignore custom width/height)
//     int width = canvas_width;
//     int height = canvas_height;
    
//     log_message(std::format("FULL-SCREEN BANNER: Browser source dimensions {}x{} (Full Canvas - Custom w/h {}x{} for CSS only)", 
//                            width, height, custom_width, custom_height));
    
//     obs_data_set_int(settings, "width", width);
//     obs_data_set_int(settings, "height", height);
//     obs_data_set_int(settings, "fps", 30);
//     obs_data_set_bool(settings, "reroute_audio", false);
//     obs_data_set_bool(settings, "restart_when_active", true); // Help with content loading
//     obs_data_set_bool(settings, "shutdown", false);
//     obs_data_set_string(settings, "css", css_content.c_str());
    
//     log_message("ENHANCED BANNER CREATION: Attempting to create browser_source directly...");
//     log_message(std::format("ENHANCED BANNER CREATION: New source URL: {}{}", url_content.substr(0, 100), (url_content.length() > 100 ? "..." : "")));
//     log_message(std::format("ENHANCED BANNER CREATION: Dimensions: {}x{}", width, height));
//     log_message(std::format("ENHANCED BANNER CREATION: CSS: {}", (custom_css.empty() ? "default" : "custom")));
    
//     // Create browser_source directly - simple and reliable
//     m_banner_source = obs_source_create_private("browser_source", m_banner_source_name.c_str(), settings);
    
//     obs_data_release(settings);
    
//     if (m_banner_source) {
//         // Add VortiDeck protection metadata to the browser source
//         obs_data_t* private_settings = obs_source_get_private_settings(m_banner_source);
//         obs_data_set_string(private_settings, "vortideck_banner", "true");
//         obs_data_set_string(private_settings, "vortideck_banner_id", "banner_v1");
//         obs_data_set_string(private_settings, "vortideck_banner_type", "browser");
//         obs_data_release(private_settings);
        
//         // Connect source signals for visibility monitoring
//         connect_source_signals();
        
//         log_message(std::format("ENHANCED BANNER CREATION: Successfully created browser_source with VortiDeck protection - Type: {}", content_type));
        
//         // Add additional verification
//         const char* source_type = obs_source_get_id(m_banner_source);
//         uint32_t actual_width = obs_source_get_width(m_banner_source);
//         uint32_t actual_height = obs_source_get_height(m_banner_source);
        
//         log_message(std::format("ENHANCED BANNER CREATION: Source verification - Type: {}, Size: {}x{}", 
//                                (source_type ? source_type : "NULL"), actual_width, actual_height));
//     } else {
//         log_message("ENHANCED BANNER CREATION: FAILED to create Browser Source for VortiDeck Banner!");
        
//         // Fallback to regular banner creation
//         log_message("ENHANCED BANNER CREATION: Falling back to regular banner creation...");
//         create_banner_source(content_data, content_type);
//     }
// }

void banner_manager::add_banner_to_current_scene()
{
    obs_source_t* current_scene_source = obs_frontend_get_current_scene();
    if (!current_scene_source) {
        log_message("No current scene");
        return;
    }

    const char* scene_name = obs_source_get_name(current_scene_source);
    if (!scene_name) {
        obs_source_release(current_scene_source);
        log_message("Could not get scene name");
        return;
    }

    obs_scene_t* scene = obs_scene_from_source(current_scene_source);
    if (!scene) {
        obs_source_release(current_scene_source);
        log_message("Could not get scene from source");
        return;
    }

    log_message("ADD BANNER: Adding banner to current scene: " + std::string(scene_name));

    // CRITICAL: Check if banner already exists in scene to prevent duplicates (metadata-based)
    int banner_count = count_vortideck_banners_in_scene(scene);
    if (banner_count > 0) {
        // Banner(s) already exist - just ensure first one is visible and properly configured
        obs_sceneitem_t* existing_item = find_vortideck_ads_in_scene(scene);
        if (existing_item) {
            obs_sceneitem_set_visible(existing_item, true);
            lock_banner_item(existing_item);
        }
        log_message(std::format("Banner already exists in scene ({} found) - NO NEW CREATION", banner_count));
    } else {
        // Get or create wrapper for this scene (NEW - prevents CEF crashes)
        obs_source_t* wrapper = get_or_create_wrapper_for_scene(scene_name);
        if (!wrapper) {
            obs_source_release(current_scene_source);
            log_message("Failed to get/create wrapper for scene");
            return;
        }

        // Add new scene item with per-scene wrapper
        // NOTE: obs_scene_add() adds its own reference, so we need to release ours
        obs_sceneitem_t* scene_item = obs_scene_add(scene, wrapper);
        obs_source_release(wrapper);  // CRITICAL: Release the reference from get_or_create_wrapper_for_scene()
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
            log_message("NEW banner added to current scene (1 per scene max) using per-scene wrapper");
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
            obs_sceneitem_t* existing_item = find_vortideck_ads_in_scene(scene);
            if (existing_item) {
                obs_sceneitem_set_visible(existing_item, true);
                lock_banner_item(existing_item);
            }
            scenes_already_covered++;
            log_message("FREE USER: Scene '" + std::string(scene_name) + "' already has " + std::to_string(banner_count) + " banner(s) - NO NEW CREATION");
        } else {
            // Scene missing banner - add single shared banner source
            if (!m_banner_source) {
                log_message("FREE USER: No banner source exists, cannot add to scene");
                continue;
            }

            // Add the single shared banner source to this scene
            obs_sceneitem_t* scene_item = obs_scene_add(scene, m_banner_source);
            if (scene_item) {
                // Position banner at 0,0
                vec2 pos = {0, 0};
                obs_sceneitem_set_pos(scene_item, &pos);

                // Make it visible and locked at top
                obs_sceneitem_set_visible(scene_item, true);
                lock_banner_item(scene_item);
                obs_sceneitem_set_order(scene_item, OBS_ORDER_MOVE_TOP);

                scenes_initialized++;
                log_message("FREE USER: Scene '" + std::string(scene_name) + "' missing banner - INITIALIZED with shared source");
            } else {
                log_message("ERROR: Failed to initialize banner in scene '" + std::string(scene_name) + "'");
            }
        }
    }
    
    obs_frontend_source_list_free(&scenes);
    
    log_message("FREE USER: Banner initialization complete - " + std::to_string(scenes_initialized) + " scenes initialized, " + 
                std::to_string(scenes_already_covered) + " scenes already covered (" + 
                std::to_string(total_scenes) + " total scenes)");
    
    // CRITICAL: After all banners are initialized, enforce lock and position for ALL scenes
    log_message("FREE USER: Enforcing banner lock and position after initialization");
    enforce_banner_lock_and_position();
}

void banner_manager::remove_banner_from_scenes()
{
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);

    int scenes_removed = 0;

    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* source = scenes.sources.array[i];
        if (!source) continue;

        obs_scene_t* scene = obs_scene_from_source(source);
        if (!scene) continue;

        // Use metadata-based detection instead of name-based
        obs_sceneitem_t* item = find_vortideck_ads_in_scene(scene);
        if (item) {
            // CRITICAL: Actually REMOVE the scene item, don't just hide it
            // Hiding leaves scene item references that prevent browser source cleanup
            obs_sceneitem_remove(item);
            scenes_removed++;
        }
    }

    obs_frontend_source_list_free(&scenes);

    // Update the visibility flag based on actual scene state
    if (scenes_removed > 0) {
        m_banner_visible = false;
        log_message("Banner removed from " + std::to_string(scenes_removed) + " scenes");
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
        
        // Always move banner to top (above all other sources)
        obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
        
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
    if (m_banner_persistent) {
        log_message("Banner already in persistent mode - ignoring duplicate request");
        return;
    }

    // Set persistent flag
    m_banner_persistent = true;

    // DON'T add to all scenes immediately - let the monitoring system handle it gradually
    // This prevents the massive banner creation burst that was happening

    log_message("Banner persistence mode ENABLED - will be maintained across scenes");

    // Signal-based monitoring handles persistence automatically
    log_message("Banner made persistent - signal-based protection active (no polling needed)");

    // Ensure signals are connected for persistence
    connect_scene_signals();
    connect_source_signals();
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
bool banner_manager::is_vortideck_ads_name(const char* name) const
{
    if (!name) return false;
    
    // Enhanced name checking - handles OBS auto-renaming patterns
    // OBS renames like: VortiDeck Banner -> VortiDeck Banner 2, VortiDeck Banner 3, etc.
    return strstr(name, "VortiDeck Banner") != nullptr;
}

bool banner_manager::is_vortideck_ads_by_metadata(obs_source_t* source) const
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

bool banner_manager::is_vortideck_ads_item(obs_sceneitem_t* item) const
{
    if (!item) return false;
    
    obs_source_t* source = obs_sceneitem_get_source(item);
    if (!source) return false;
    
    // Method 1: Check metadata first (most reliable)
    if (is_vortideck_ads_by_metadata(source)) {
        return true;
    }
    
    // Method 2: Fallback to name-based detection (for existing banners without metadata)
    const char* source_name = obs_source_get_name(source);
    return is_vortideck_ads_name(source_name);
}

obs_sceneitem_t* banner_manager::find_vortideck_ads_in_scene(obs_scene_t* scene) const
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
        if (fd->manager->is_vortideck_ads_item(item)) {
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
        if (cd->manager->is_vortideck_ads_item(item)) {
            cd->count++;
        }
        return true; // Continue enumeration
    }, &data);
    
    return data.count;
}

void banner_manager::cleanup_banner_names_after_deletion()
{
    // CRITICAL: Check shutdown flag FIRST before enumerating scenes
    if (m_shutting_down.load()) {
        log_message("CLEANUP: Shutdown detected, aborting cleanup");
        return;
    }

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



// REMOVED: Deprecated polling monitor - now using signal-based monitoring only
// Signals provide instant reaction and better performance

void banner_manager::stop_persistence_monitor()
{
    // Legacy function kept for compatibility - no longer creates polling threads
    m_persistence_monitor_active = false;
    m_banner_persistent = false;
    log_message("Legacy persistence monitor stopped (now using signals)");
}

void banner_manager::enforce_banner_visibility()
{
    // CRITICAL: Check shutdown flag FIRST before touching OBS API
    if (m_shutting_down.load()) {
        log_message("PERSISTENCE: Shutdown detected, aborting enforcement");
        return;
    }

    // CRITICAL: Don't enforce during intentional hide operations
    if (m_intentional_hide_in_progress.load()) {
        log_message("PERSISTENCE: Skipping enforcement - intentional hide in progress");
        return;
    }

    // Don't enforce if banner is supposed to be hidden due to frequency limits
    // Ad frequency limits removed - handled externally
    // Persistence enforcement always active for applicable users

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

        // Get scene name for per-scene wrapper lookup
        const char* scene_name = obs_source_get_name(scene_source);
        if (!scene_name) continue;

        // Use metadata-based detection instead of name-based
        obs_sceneitem_t* banner_item = find_vortideck_ads_in_scene(scene);
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
                    log_message("FREE USER: Banner missing in current scene - recreating with per-scene wrapper");

                    // Get or create wrapper for this scene (NEW - prevents CEF crashes)
                    obs_source_t* wrapper = get_or_create_wrapper_for_scene(scene_name);
                    if (!wrapper) {
                        log_message("FREE USER: Failed to get/create wrapper for scene: " + std::string(scene_name));
                        if (current_scene_source) {
                            obs_source_release(current_scene_source);
                        }
                        continue;
                    }

                    // NOTE: obs_scene_add() adds its own reference, so we need to release ours
                    obs_sceneitem_t* scene_item = obs_scene_add(scene, wrapper);
                    obs_source_release(wrapper);  // CRITICAL: Release the reference from get_or_create_wrapper_for_scene()
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
                        log_message("FREE USER: Banner restored in current scene with per-scene wrapper");
                    }
                } else if (m_banner_persistent) {
                    log_message("PREMIUM USER: Banner missing but in persistent mode - recreating with per-scene wrapper");

                    // Get or create wrapper for this scene (NEW - prevents CEF crashes)
                    obs_source_t* wrapper = get_or_create_wrapper_for_scene(scene_name);
                    if (!wrapper) {
                        log_message("PREMIUM USER: Failed to get/create wrapper for scene: " + std::string(scene_name));
                        if (current_scene_source) {
                            obs_source_release(current_scene_source);
                        }
                        continue;
                    }

                    // NOTE: obs_scene_add() adds its own reference, so we need to release ours
                    obs_sceneitem_t* scene_item = obs_scene_add(scene, wrapper);
                    obs_source_release(wrapper);  // CRITICAL: Release the reference from get_or_create_wrapper_for_scene()
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
        obs_sceneitem_t* banner_item = find_vortideck_ads_in_scene(scene);
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
    // Register VortiDeck ADS as a custom source type that creates browser sources internally
    struct obs_source_info vortideck_banner_info = {};
    vortideck_banner_info.id = vortideck::SOURCE_ID_ADS;
    vortideck_banner_info.type = OBS_SOURCE_TYPE_INPUT;
    vortideck_banner_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    
    vortideck_banner_info.get_name = [](void* unused) -> const char* {
        UNUSED_PARAMETER(unused);
        return "VortiDeck ADS";
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
            obs_data_set_string(private_settings, vortideck::META_TYPE, "ads");
            obs_data_release(private_settings);
    
            log_to_obs("VortiDeck ADS: Created browser source wrapper");
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
            
            log_to_obs("VortiDeck ADS: Updated browser source settings");
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
    log_to_obs("VortiDeck ADS: Registered as custom source (creates browser sources internally)");
}

void banner_manager::unregister_vortideck_banner_source()
{
    // OBS automatically handles unregistration when the module unloads
    log_to_obs("VortiDeck Banner: Source unregistered");
}

// Custom source callbacks removed - using browser_source directly for full compatibility

// ============================================================================
// BANNER QUEUE MANAGEMENT IMPLEMENTATION - REMOVED
// ============================================================================
// All queue and rotation methods have been removed as they are now handled
// by the external application















// can_show_ad_now() method removed - frequency control handled externally



// Helper function to handle banner hiding with user type restrictions
void banner_manager::hide_banner_with_user_restrictions(const std::string& reason)
{
    if (is_premium_user()) {
        // PREMIUM: Can hide banners
        log_message("BANNER_HIDE: PREMIUM USER - Hiding banner (" + reason + ")");
        remove_banner_from_scenes();
        m_banner_visible = false;
    } else {
        // FREE: Show connected service banner instead of hiding (maintain banner source visibility)
        log_message("BANNER_HIDE: FREE USER - Showing connected service banner (" + reason + ") - cannot hide banners");
        create_banner_source();
        // Keep m_banner_visible = true for free users (banner source remains visible)
    }
}

