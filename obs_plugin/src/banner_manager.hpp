#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <libobs/obs.h>
#include <libobs/obs-source.h>
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

            // OBS Source Registration (new)
            static void register_vortideck_banner_source();
            static void unregister_vortideck_banner_source();

            // Initialization (call after OBS is fully loaded)
            void initialize_after_obs_ready();  // CRITICAL: Call this after OBS is fully initialized
            void enable_signal_connections_when_safe();  // SAFE: Enable signals when OBS frontend is ready
            void cleanup_for_scene_collection_change();  // Clean up sources before scene collection changes
            
            // Main banner operations
            void show_banner(bool enable_duration_timer = true);
            void hide_banner();
            void toggle_banner();
                    void set_banner_content(std::string_view content_data, std::string_view content_type);
        void set_banner_content_with_custom_params(std::string_view content_data, std::string_view content_type,
                                                   std::string_view custom_css = "", int custom_width = 0, int custom_height = 0, bool css_locked = false);
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
            
            // Premium status and monetization
            void update_premium_status(const nlohmann::json& message);
            bool is_premium_user() const;
            float get_revenue_share() const;
            
            // NEW: Ad Duration & Queue Management
            struct AdWindow {
                std::string id;
                std::string content_data;
                std::string content_type;
                std::string css;
                int duration_seconds;
                std::chrono::system_clock::time_point start_time;
                std::chrono::system_clock::time_point end_time;
                bool auto_rotate;
                std::string next_banner_id;
            };

            struct AdQueue {
                std::vector<AdWindow> banners;
                std::atomic<size_t> current_index{0};
                std::atomic<bool> auto_rotation{true};
                std::atomic<int> default_duration{30};
                std::mutex queue_mutex;
            };

            // Queue management methods
            void set_ad_queue(const std::vector<AdWindow>& queue);
            void add_ad_to_queue(const AdWindow& ad);
            void start_ad_rotation();
            void stop_ad_rotation();
            void rotate_to_next_ad();
            bool is_current_ad_expired() const;
            void set_ad_duration_limit(int max_seconds);
            
            // Auto-rotation control
            void enable_auto_rotation(bool enable = true);
            bool is_auto_rotation_enabled() const;
            
            // Window lifecycle
                    void start_ad_window(std::string_view window_id);
        void close_ad_window(std::string_view window_id);
            void auto_close_expired_windows();
            
                // Duration control helpers
    int get_effective_ad_duration(int requested_duration) const;
    int get_max_duration_for_user_type() const;
    bool should_cap_ad_duration(int requested_duration) const;
    
    // Window management
    bool can_ad_fit_in_current_window(int ad_duration) const;
    void close_current_window();
    void start_new_window();
    int get_remaining_window_time() const;
    void reset_window_tracking();
    
    // Gap management between windows
    void start_window_gap();
    void stop_window_gap();
    bool is_in_window_gap() const;
    bool can_start_new_window() const;
            
            // Public access to banner source name (needed for menu callback)
            const std::string m_banner_source_name;

            // Analytics tracking methods (public for obs_plugin access)
            void track_ad_display_start(const std::string& ad_id, const std::string& campaign_id, 
                                       int planned_duration_ms, const std::string& content_type);
            void track_ad_display_end(const std::string& ad_id, int actual_duration_ms);
            void track_viewer_metrics(int estimated_viewers, double bitrate, double framerate);
            void send_batch_analytics_report();
            void enable_analytics_tracking(bool enable, int report_interval_seconds = 60);
            int calculate_current_viewer_estimate();
            
            // Duration control for immediate mode
            void set_custom_banner_duration(int duration_seconds);
            
            // Ad frequency control
            bool can_show_ad_now() const;
            void start_duration_timer(int duration_seconds);
            void stop_duration_timer();

        private:
            // Helper function to handle banner hiding with user type restrictions
            void hide_banner_with_user_restrictions(const std::string& reason);
            
            // Custom source callbacks removed - using browser_source directly

            // Signal-based immediate enforcement (ACTIVE implementation)
            void connect_scene_signals();
            void disconnect_scene_signals(); 
            static void on_item_remove(void* data, calldata_t* calldata);
            static void on_item_visible(void* data, calldata_t* calldata);
            static void on_item_transform(void* data, calldata_t* calldata);
            
            // Safety checks
            bool is_obs_safe_to_call() const;
            
            // Internal operations
                    void create_banner_source(std::string_view content_data, std::string_view content_type);
        void create_banner_source_with_custom_params(std::string_view content_data, std::string_view content_type,
                                                     std::string_view custom_css, int custom_width, int custom_height, bool css_locked = false);
            void add_banner_to_current_scene();
            void initialize_banners_all_scenes();  // Initialize banners across ALL scenes (for FREE USERS)
            void remove_banner_from_scenes();
            void lock_banner_item(obs_sceneitem_t* item);
            
            // Content type detection and handling
            bool is_url(std::string_view content);
            bool is_file_path(std::string_view content);
            bool is_image_content(std::string_view content_type);
            bool is_video_content(std::string_view content_type);
            
            // Better banner detection using metadata + fallback to name checking
            bool is_vortideck_banner_name(const char* name) const;
            bool is_vortideck_banner_by_metadata(obs_source_t* source) const;
            bool is_vortideck_banner_item(obs_sceneitem_t* item) const;
            obs_sceneitem_t* find_vortideck_banner_in_scene(obs_scene_t* scene) const;
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
                
                // Get revenue share for current user
                static float get_revenue_share(const banner_manager* manager) {
                    return manager->m_revenue_share.load();
                }
                
                // Get ad frequency for current user
                static int get_ad_frequency_minutes(const banner_manager* manager) {
                    return manager->m_ad_frequency_minutes.load();
                }
                
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
            obs_source_t* m_banner_source;
            bool m_banner_visible;
            bool m_banner_persistent;  // True when banner should be unhideable
            bool m_persistence_monitor_active;  // Monitor jthread active flag
            std::atomic<bool> m_shutting_down{false};  // Shutdown flag for thread coordination
            std::atomic<bool> m_browser_shutdown_initiated{false};  // CRITICAL: Flag to prevent duplicate CEF shutdown
            std::condition_variable m_shutdown_cv;       // Condition variable for immediate thread wake-up during shutdown
            
            // Thread management for temporary operations
            std::vector<std::jthread> m_temporary_threads;
            std::mutex m_threads_mutex;
            std::string m_current_banner_content;
            std::string m_current_content_type;
            
            // Premium and monetization state (jthread-safe)
            std::atomic<bool> m_is_premium{false};
            std::atomic<float> m_revenue_share{0.05f};  // 5% for free creators
            std::atomic<int> m_ad_frequency_minutes{5};  // Every 5 mins for free
            std::atomic<bool> m_custom_positioning{false};
            std::mutex m_premium_mutex;
            std::chrono::system_clock::time_point m_last_premium_update;
            
            // NEW: Ad Queue Management
            AdQueue m_ad_queue;
            std::atomic<bool> m_rotation_active{false};
            std::atomic<int> m_max_ad_duration{60};  // 60 seconds default
            std::jthread m_rotation_jthread;  // C++20 jthread with built-in stop_token
            
                // Duration control
    std::chrono::system_clock::time_point m_current_ad_start;
                std::atomic<int> m_current_ad_duration{30};
            std::atomic<int> m_custom_banner_duration{0}; // 0 = use default duration
            std::jthread m_duration_timer_thread; // Single timer for duration control
            std::atomic<bool> m_duration_timer_active{false};
    
    // Window tracking
    std::chrono::system_clock::time_point m_current_window_start;
    std::atomic<int> m_current_window_used_time{0};
                std::atomic<bool> m_window_active{false};
    
    // Gap between windows (15 seconds for testing, 3 minutes for production)
    std::atomic<bool> m_in_window_gap{false};
    std::chrono::system_clock::time_point m_window_gap_start;
    std::jthread m_window_gap_thread;
    std::jthread m_persistence_monitor_thread;
            
            // Ad frequency control for free users
            std::chrono::system_clock::time_point m_last_ad_end_time;
            std::atomic<bool> m_ad_frequency_enforced{true};
            
            // Signal loop prevention
            std::atomic<bool> m_correcting_position{false};  // Flag to prevent infinite loops
            
            // Initialization prevention
            std::atomic<bool> m_initialization_started{false};  // Flag to prevent multiple initialization calls
            std::atomic<bool> m_signals_connected{false};  // Flag to prevent multiple signal connections
            
            // Add flag to temporarily disable signal handling during intentional operations
            std::atomic<bool> m_intentional_hide_in_progress{false};
            
            // Analytics tracking structures
            struct AdDisplayMetrics {
                std::string ad_id;
                std::string campaign_id;
                std::chrono::system_clock::time_point start_time;
                std::chrono::system_clock::time_point end_time;
                int planned_duration_ms;
                int actual_duration_ms;
                int viewer_count_at_display;
                double completion_percentage;
                bool premium_user;
                float revenue_share;
                std::string content_type;
                size_t content_size_bytes;
            };
            
            struct ViewerMetrics {
                std::chrono::system_clock::time_point timestamp;
                int estimated_viewers;
                int twitch_viewers;
                int youtube_viewers;
                int facebook_viewers;
                double stream_bitrate;
                double stream_framerate;
                double stream_quality_score;
            };
            
            struct AnalyticsState {
                std::atomic<int> total_displays{0};
                std::atomic<int> total_duration_ms{0};
                std::atomic<double> total_revenue{0.0};
                std::atomic<int> average_viewer_count{0};
                std::atomic<bool> tracking_enabled{true};
                std::atomic<int> report_interval_seconds{60};
                std::vector<AdDisplayMetrics> display_history;
                std::vector<ViewerMetrics> viewer_history;
                std::mutex analytics_mutex;
                std::chrono::system_clock::time_point last_report_time;
            };
            
            // Analytics tracking methods (remaining private)
            void update_platform_viewer_count(const std::string& platform, int viewer_count);
            void send_analytics_report();
            void send_display_analytics(const AdDisplayMetrics& metrics);
            AdDisplayMetrics* find_active_ad_display(const std::string& ad_id);
            double calculate_completion_percentage(int actual_duration, int planned_duration);
            
            // External analytics integration
            void send_google_analytics_event(const std::string& event_category, 
                                           const std::string& event_action, 
                                           const std::string& event_label,
                                           const nlohmann::json& custom_metrics);
            void send_facebook_pixel_event(const std::string& event_name, 
                                         const nlohmann::json& event_parameters);
            
            // Platform integration helpers
            void request_platform_viewer_data();
            void integrate_twitch_metrics(const nlohmann::json& twitch_data);
            void integrate_youtube_metrics(const nlohmann::json& youtube_data);
            void integrate_facebook_metrics(const nlohmann::json& facebook_data);
            
            // Analytics state
            AnalyticsState m_analytics;
            std::atomic<bool> m_analytics_reporting_active{false};
            std::jthread m_analytics_thread;
            std::atomic<bool> m_stop_analytics{false};
            
            // Current display tracking
            std::string m_current_ad_id;
            std::string m_current_campaign_id;
            std::chrono::system_clock::time_point m_current_display_start;
            std::atomic<int> m_current_planned_duration{0};
            
            // Viewer tracking
            std::atomic<int> m_current_twitch_viewers{0};
            std::atomic<int> m_current_youtube_viewers{0};
            std::atomic<int> m_current_facebook_viewers{0};
            std::atomic<int> m_estimated_total_viewers{0};
            
            // Logging helper
            void log_message(std::string_view message) const;
            
            // Thread management helpers
            void start_temporary_thread(std::function<void()> task);
            void cleanup_finished_threads();
            void stop_all_threads();
            
            // Static thread helper for signal callbacks (uses weak pointer pattern)
            static void start_safe_signal_thread(banner_manager* manager, std::function<void(banner_manager*)> task);
        };

        // Custom source data structure removed - using browser_source directly
    }
} 