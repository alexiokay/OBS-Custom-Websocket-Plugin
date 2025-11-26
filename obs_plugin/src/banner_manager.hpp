#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <obs.h>
#include <obs-source.h>
// Note: signal_handler functions are included in obs.h

// Include JSON directly instead of forward declaration
#include <nlohmann/json.hpp>

namespace vorti {
    namespace applets {
        class banner_manager {
        public:
            banner_manager();
            ~banner_manager();

            // Shutdown method for explicit cleanup (call before destruction)
            void shutdown();

            // Early shutdown flag (call FIRST in obs_module_unload to stop signal handlers immediately)
            void set_shutting_down();

            // Early signal disconnection (call in obs_module_unload before source teardown)
            void disconnect_all_signals();

            // OBS Source Registration (new)
            static void register_vortideck_banner_source();
            static void unregister_vortideck_banner_source();

            // Initialization (call after OBS is fully loaded)
            void initialize_after_obs_ready();  // CRITICAL: Call this after OBS is fully initialized
            void enable_signal_connections_when_safe();  // SAFE: Enable signals when OBS frontend is ready
            void cleanup_for_scene_collection_change();  // Clean up sources before scene collection changes
            
            // Main banner operations (simplified)
            void show_banner(bool enable_duration_timer = false);
            void hide_banner();
            void toggle_banner();
            void set_banner_url(const std::string& url);  // Simple URL-only method like overlays
            void resize_banner_to_canvas();  // Resize banner to match current canvas size
            void show_premium_banner();  // PREMIUM USERS ONLY: Add banner to current scene by choice
            void make_banner_persistent();  // Make banner unhideable and persistent
            
            // State queries
            bool is_banner_visible() const;
            std::string get_current_banner_content() const;
            obs_source_t* get_banner_source() const;
            
            // Menu integration
            void add_banner_menu();
            static void banner_menu_callback(void* data);
            
            // Persistence monitoring
            void start_persistence_monitor();
            void stop_persistence_monitor();
            static void persistence_timer_callback(void* data);
            void enforce_banner_visibility();
            void force_refresh_banner_visibility();
            
            // Premium status (simplified)
            void update_premium_status(const nlohmann::json& message);
            bool is_premium_user() const;
            float get_revenue_share() const;
            
            // Ad Queue Management removed - handled by external application

            // Queue management methods removed - handled by external application
            
            // Window lifecycle and duration control removed - handled by external application
            
            // Public access to banner source name (needed for menu callback)
            const std::string m_banner_source_name;

            // Analytics tracking methods removed - handled by external application
            
            // Duration control removed - handled by external application
            
            // Methods removed but still called in code - adding stubs
            void set_custom_banner_duration(int /*duration_seconds*/) { /* No-op */ }
            void track_ad_display_end(const std::string& /*ad_id*/, int /*actual_duration_ms*/) { /* No-op */ }
            bool is_auto_rotation_enabled() const { return false; }
            void enable_auto_rotation(bool /*enable*/ = true) { /* No-op */ }

        private:
            // Helper function to handle banner hiding with user type restrictions
            void hide_banner_with_user_restrictions(const std::string& reason);
            
            // Custom source callbacks removed - using browser_source directly

            // Signal-based immediate enforcement (ACTIVE implementation)
            void connect_scene_signals();
            void disconnect_scene_signals();
            void connect_source_signals();
            void disconnect_source_signals();

            // Global OBS signal handlers (prevents CEF crash - no scene signals)
            static void on_global_source_remove(void* data, calldata_t* calldata);

            // Old scene signal handlers (kept for reference but may cause CEF crash)
            static void on_item_add(void* data, calldata_t* calldata);
            static void on_item_remove(void* data, calldata_t* calldata);
            static void on_item_visible(void* data, calldata_t* calldata);
            static void on_item_transform(void* data, calldata_t* calldata);

            // Source signal handlers (for banner source itself)
            static void on_source_hide(void* data, calldata_t* calldata);
            static void on_source_show(void* data, calldata_t* calldata);
            static void on_source_deactivate(void* data, calldata_t* calldata);
            static void on_source_activate(void* data, calldata_t* calldata);
            
            // Scene item order monitoring (for free users)
            static void on_scene_reorder(void* data, calldata_t* calldata);
            
            // Prevention-based banner enforcement (better than correction)
            void enforce_banner_lock_and_position();
            
            // Internal operations (simplified - always uses connected service URL)
            void create_banner_source(std::string_view content_data = "", std::string_view content_type = "");
            void add_banner_to_current_scene();
            void initialize_banners_all_scenes();  // Initialize banners across ALL scenes (for FREE USERS)
            void remove_banner_from_scenes();
            void lock_banner_item(obs_sceneitem_t* item);

            // Per-scene wrapper management (NEW - prevents CEF crashes)
            obs_source_t* get_or_create_wrapper_for_scene(const std::string& scene_name);
            void release_wrapper_for_scene(const std::string& scene_name);
            void release_all_wrappers();
            
            // Content type detection and handling
            bool is_url(std::string_view content);
            bool is_file_path(std::string_view content);
            bool is_image_content(std::string_view content_type);
            bool is_video_content(std::string_view content_type);
            
            // Better banner detection using metadata + fallback to name checking
            bool is_vortideck_ads_name(const char* name) const;
            bool is_vortideck_ads_by_metadata(obs_source_t* source) const;
            bool is_vortideck_ads_item(obs_sceneitem_t* item) const;
            obs_sceneitem_t* find_vortideck_ads_in_scene(obs_scene_t* scene) const;
            int count_vortideck_banners_in_scene(obs_scene_t* scene) const;
            
            // Banner cleanup after deletion
            void cleanup_banner_names_after_deletion();
            void rename_source_if_needed(obs_source_t* source, std::string_view target_name);
            
            // Premium status and monetization
            struct PremiumInfo {
                bool is_premium = false;
                float revenue_share = 0.05f;  // 5% for free creators
                int ad_frequency_minutes = 5;  // VortiDeck ads every 5 mins for free
                bool custom_positioning = false;
                std::chrono::system_clock::time_point last_updated;
            };

            // NEW: Centralized Premium Status Handler
            class PremiumStatusHandler {
            public:
                // Check if user is premium
                static bool is_premium(const banner_manager* manager) {
                    return manager->m_is_premium.load();
                }
                
                // Check if a specific action is allowed for current user type
                static bool is_action_allowed(const banner_manager* manager, const std::string& action_type) {
                    bool is_premium = manager->m_is_premium.load();
                    
                    // Premium actions that are restricted for free users
                    if (action_type == "banner_hide" || action_type == "banner_remove" || 
                        action_type == "banner_modify" || action_type == "banner_position" ||
                        action_type == "banner_css_edit" || action_type == "banner_duration_control") {
                        return is_premium;
                    }
                    
                    // Actions allowed for all users
                    return true;
                }
                
                // Get user type as string for logging
                static std::string get_user_type_string(const banner_manager* manager) {
                    return manager->m_is_premium.load() ? "premium" : "free";
                }
                
                // Revenue share and ad frequency removed - handled by external application
                
                // Check if custom positioning is allowed
                static bool can_customize_positioning(const banner_manager* manager) {
                    return manager->m_custom_positioning.load();
                }
                
                // Log premium-related actions
                static void log_premium_action(const banner_manager* manager, const std::string& action, 
                                             const std::string& result) {
                    std::string user_type = get_user_type_string(manager);
                    std::string log_msg = "PREMIUM: " + user_type + " user - " + action + " - " + result;
                    manager->log_message(log_msg);
                }
                
                // Handle premium restriction with automatic logging
                static bool handle_premium_restriction(const banner_manager* manager, const std::string& action_type, 
                                                     const std::string& action_description) {
                    if (!is_action_allowed(manager, action_type)) {
                        log_premium_action(manager, action_description, "DENIED (premium required)");
                        return false;
                    }
                    
                    log_premium_action(manager, action_description, "ALLOWED");
                    return true;
                }
            };
            
            // State variables
            obs_source_t* m_banner_source;  // RESTORED: Single shared banner source across all scenes

            bool m_banner_visible;
            bool m_banner_persistent;  // True when banner should be unhideable
            bool m_persistence_monitor_active;  // Monitor jthread active flag (to be removed)
            std::atomic<bool> m_source_visible{false};  // Cached visibility state from signals
            std::atomic<bool> m_shutting_down{false};  // Shutdown flag for thread coordination
            std::string m_current_banner_content;
            std::string m_current_content_type;
            
            // Premium state (simplified)
            std::atomic<bool> m_is_premium{false};
            std::atomic<float> m_revenue_share{0.05f};  // 5% for free creators
            std::atomic<int> m_ad_frequency_minutes{5};  // Every 5 mins for free
            std::atomic<bool> m_custom_positioning{false};
            std::mutex m_premium_mutex;
            std::chrono::system_clock::time_point m_last_premium_update;
            
            // Ad Queue Management removed - handled by external application
            
            // Duration control, window tracking, and ad frequency control removed - handled by external application
            
            // Signal loop prevention
            std::atomic<bool> m_correcting_position{false};  // Flag to prevent infinite loops
            
            // Initialization prevention
            std::atomic<bool> m_initialization_started{false};  // Flag to prevent multiple initialization calls
            std::atomic<bool> m_signals_connected{false};  // Flag to prevent multiple signal connections

            // Add flag to temporarily disable signal handling during intentional operations
            std::atomic<bool> m_intentional_hide_in_progress{false};
            std::atomic<bool> m_cleanup_in_progress{false};  // Flag to prevent restoration during cleanup

            // Polling thread for banner restoration (NO signals to prevent CEF crash)
            std::jthread m_polling_thread;  // Managed thread that can be stopped/joined during shutdown

            // Timer-based banner enforcement
            std::atomic<bool> m_enforcement_timer_active{false};
            obs_weak_source_t* m_timer_source{nullptr};
            
            // Analytics structures removed - handled by external application
            
            // Analytics methods removed - handled by external application
            
            // Analytics state removed - handled by external application
            
            // Ad frequency control for free users (simplified)
            std::chrono::system_clock::time_point m_last_ad_end_time;
            
            // Logging helper
            void log_message(std::string_view message) const;
        };

        // Custom source data structure removed - using browser_source directly
    }
} 