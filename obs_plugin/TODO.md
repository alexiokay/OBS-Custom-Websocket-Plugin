# VortiDeck OBS Plugin - Development TODO

## 🔥 **HIGH PRIORITY**

### 🧹 **Code Cleanup & Legacy Removal**

- [ ] **Fix Content Type Detection Function Usage**
  - **Location**: `src/banner_manager.cpp` lines 1460-1490 (`vortideck_banner_update` function)
  - **Functions to USE (not remove)**:
    - `bool is_url(std::string_view content)` - **NEEDED for URL vs file detection**
    - `bool is_file_path(std::string_view content)` - **NEEDED for local file handling**
    - `bool is_image_content(std::string_view content_type)` - **NEEDED for image ad detection**
    - `bool is_video_content(std::string_view content_type)` - **NEEDED for video ad support**
  - **Problem**: Currently using basic string comparisons instead of these utility functions
  - **Impact**: Cleaner code, better content validation, essential for ad loading
  - **Fix**: Update `vortideck_banner_update` to use these functions properly

### 🎯 **Premium Status System Refactor**

- [ ] **Create Common Premium Status Handler**
  ```cpp
  // New file: src/premium_status_handler.hpp
  class premium_status_handler {
  public:
      static bool extract_and_update_premium_status(
          const nlohmann::json& message, 
          banner_manager& banner_mgr
      );
      static bool is_premium_action_allowed(
          std::string_view action_id, 
          bool is_premium
      );
      static void log_premium_action(
          std::string_view action_id, 
          bool is_premium, 
          bool allowed
      );
  };
  ```

- [ ] **Add Common Premium Status Parameter to All Actions**
  - **Target**: All action handlers in `src/obs_plugin.cpp`
  - **Implementation**:
    ```cpp
    // Add to every action handler:
    void action_*(...parameters) {
        // Extract premium status from message (if present)
        bool status_updated = premium_status_handler::extract_and_update_premium_status(
            parameters, m_banner_manager
        );
        
        bool is_premium = m_banner_manager.is_premium_user();
        bool action_allowed = premium_status_handler::is_premium_action_allowed(
            action_id, is_premium
        );
        
        if (!action_allowed) {
            premium_status_handler::log_premium_action(action_id, is_premium, false);
            return;
        }
        
        // Continue with action logic...
    }
    ```

- [ ] **Replace Hardcoded Premium Checks**
  - **Current**: 19+ instances of `m_is_premium.load()` scattered throughout code
  - **Target**: Centralize through helper functions
  - **Files to Update**:
    - `src/banner_manager.cpp` (17 instances)
    - `src/obs_plugin.cpp` (2 instances)

---

## 🔧 **MEDIUM PRIORITY**

### 🔄 **Banner Queue Implementation**

- [ ] **Add Banner Queue Data Structure**
  ```cpp
  // Add to banner_manager.hpp
  struct BannerContent {
      std::string type;        // "color", "text", "image", "partnership"
      std::string data;        // Content data
      int duration_seconds;    // Display duration
      std::string campaign_id; // For analytics
  };
  
  std::vector<BannerContent> m_banner_queue;
  std::atomic<size_t> m_current_banner_index{0};
  std::atomic<int> m_rotation_interval_seconds{30};
  std::mutex m_queue_mutex;
  ```

- [ ] **Add Queue Action Constants**
  - **Location**: `src/obs_plugin.hpp` - actions namespace
  - **Implementation**:
    ```cpp
    const std::string s_banner_set_queue = "obs_banner_set_queue";
    const std::string s_banner_add_to_queue = "obs_banner_add_to_queue";
    ```

- [ ] **Implement Queue Management Methods**
  - **Location**: `src/banner_manager.cpp`
  - **Methods to implement**:
    ```cpp
    void set_ad_queue(const std::vector<AdWindow>& queue);
    void add_ad_to_queue(const AdWindow& ad);
    void show_next_ad();
    void close_current_window();
    ```

- [ ] **Implement Window Duration Tracking**
  - **Location**: `src/banner_manager.cpp`
  - **Features**:
    - Fair duration management (no truncated ads)
    - Window lifecycle tracking
    - Ad display time validation
    - Queue rotation logic

- [ ] **Add WebSocket Action Handlers**
  - **Location**: `src/obs_plugin.cpp`
  - **Handlers to implement**:
    ```cpp
    void action_banner_set_queue(const action_invoke_parameters& parameters);
    void action_banner_add_to_queue(const action_invoke_parameters& parameters);
    ```
  - **Add to dispatcher in message handling**

- [ ] **Register Queue Actions with VortiDeck**
  - **Location**: `src/obs_plugin.cpp` - register_regular_actions()
  - **Add action registration**:
    ```cpp
    actions.push_back(register_action(actions::s_banner_set_queue, "APPLET_OBS_BANNER_SET_QUEUE"));
    actions.push_back(register_action(actions::s_banner_add_to_queue, "APPLET_OBS_BANNER_ADD_TO_QUEUE"));
    ```

- [ ] **Implement Banner Rotation Timer**
  ```cpp
  // Add rotation logic
  void start_banner_rotation();
  void stop_banner_rotation();
  void rotate_to_next_banner();
  static void rotation_timer_callback(void* data);
  ```

- [ ] **Add Queue Management API**
  ```cpp
  void set_banner_queue(const std::vector<BannerContent>& queue);
  void add_banner_to_queue(const BannerContent& banner);
  void clear_banner_queue();
  size_t get_queue_size() const;
  BannerContent get_current_banner() const;
  ```

- [ ] **Test Complete Queue System**
  - **Test scenarios**:
    - 3-5 ad batch initialization
    - Queue refill when low (≤1 ad remaining)
    - Fair duration management
    - Window lifecycle tracking
    - Premium vs free user queue handling

### 📡 **API Enhancement**

- [ ] **Add New WebSocket Actions**
  - `banner_set_queue` - Set multiple banners for rotation
  - `banner_set_rotation_speed` - Control rotation timing (Premium only)
  - `banner_get_status` - Return current banner and queue status
  - `banner_set_partnerships` - Set partnership content (Premium only)

- [ ] **Enhance Action Parameter Validation**
  ```cpp
  bool validate_action_parameters(
      std::string_view action_id,
      const action_invoke_parameters& parameters,
      bool is_premium
  );
  ```

---

## 🚀 **LOW PRIORITY / FUTURE ENHANCEMENTS**

### 📊 **Analytics & Monitoring**

- [ ] **Add Banner Analytics**
  - Track banner display time
  - Monitor rotation effectiveness
  - Log premium status changes
  - Export analytics data via API

- [ ] **Performance Monitoring**
  ```cpp
  struct BannerMetrics {
      std::chrono::steady_clock::time_point last_rotation;
      size_t total_rotations;
      std::chrono::milliseconds avg_rotation_time;
      size_t failed_rotations;
  };
  ```

### 🎨 **Content Enhancement**

- [ ] **Add Animation Support**
  - Fade transitions between banners
  - Slide animations
  - Custom transition timing

- [ ] **Advanced Positioning**
  - Multi-zone banner support
  - Corner positioning options
  - Custom anchor points

### 🔒 **Security & Reliability**

- [ ] **Enhanced Error Handling**
  - Graceful degradation when content fails to load
  - Automatic fallback to default banners
  - Connection loss recovery

- [ ] **Content Validation**
  - File size limits
  - Content type verification
  - Malformed data handling

---

## 📋 **SPECIFIC IMPLEMENTATION TASKS**

### 🔧 **Premium Status Refactor Implementation**

1. **Create Premium Status Handler** (`src/premium_status_handler.hpp`)
   ```cpp
   #pragma once
   #include <nlohmann/json.hpp>
   #include "banner_manager.hpp"
   
   namespace vorti {
       namespace applets {
           class premium_status_handler {
           public:
               // Extract premium status from any action message
               static bool extract_and_update_premium_status(
                   const nlohmann::json& message, 
                   banner_manager& banner_mgr
               );
               
               // Check if action is allowed for user type
               static bool is_premium_action_allowed(
                   std::string_view action_id, 
                   bool is_premium
               );
               
               // Log premium-related actions
               static void log_premium_action(
                   std::string_view action_id, 
                   bool is_premium, 
                   bool allowed
               );
               
               // Get user type string
               static std::string get_user_type_string(bool is_premium);
               
           private:
               // Premium-only actions list
               static const std::set<std::string> PREMIUM_ONLY_ACTIONS;
           };
       }
   }
   ```

2. **Update All Action Handlers**
   - Add common premium status extraction at start of each action
   - Replace direct `m_is_premium.load()` calls with helper functions
   - Add consistent logging for premium actions

3. **Create Banner Queue System**
   - Implement rotation timer jthread
   - Add queue management functions
   - Update WebSocket API to support queue operations

### 🧹 **Content Type Detection Improvement Steps**

1. **Update vortideck_banner_update Function**
   ```cpp
   // CURRENT (basic string comparison):
   if (std::string(content_type) == "color") {
       // Color logic
   } else if (std::string(content_type).find("image") != std::string::npos) {
       // Image logic  
   }
   
   // IMPROVED (using utility functions):
   if (std::string(content_type) == "color") {
       // Color logic
   } else if (std::string(content_type) == "text") {
       // Text logic
   } else if (is_image_content(content_type)) {
       if (is_url(content_data)) {
           // Handle image URLs (partnerships, remote ads)
       } else {
           // Handle local image files
       }
   } else if (is_video_content(content_type)) {
       if (is_url(content_data)) {
           // Handle video URLs (video partnerships)
       } else {
           // Handle local video files
       }
   }
   ```

2. **Enhance Content Type Detection for Ads**
   ```cpp
   // Add support for partnership content types
   bool is_partnership_content(std::string_view content_type);
   bool is_campaign_content(std::string_view content_type);
   bool is_network_ad_content(std::string_view content_type);
   ```

2. **Update Documentation**
   - Remove references to old content detection in docs
   - Update API documentation with new queue system
   - Add premium status parameter to all action examples

---

## 🎯 **SUCCESS CRITERIA**

### ✅ **Code Quality**
- [ ] Proper use of content type detection utility functions
- [ ] Consistent premium status handling across all actions
- [ ] Single source of truth for premium checks
- [ ] Comprehensive error handling

### ✅ **Functionality**
- [ ] Banner queue rotation works smoothly
- [ ] Premium status updates triggered from any action message
- [ ] All actions respect premium permissions
- [ ] Graceful degradation when VortiDeck app disconnects

### ✅ **Performance**
- [ ] No memory leaks in rotation system
- [ ] Efficient queue management
- [ ] Minimal CPU impact during rotations
- [ ] Fast premium status updates

---

## 📅 **ESTIMATED TIMELINE**

| Priority | Task Category | Estimated Time | Dependencies |
|----------|---------------|----------------|--------------|
| **HIGH** | Content Type Detection Fix | 1-2 hours | None |
| **HIGH** | Premium Status Refactor | 6-8 hours | Content Type Detection Fix |
| **MEDIUM** | Banner Queue System | 8-12 hours | Premium Status Refactor |
| **MEDIUM** | API Enhancement | 4-6 hours | Banner Queue System |
| **LOW** | Analytics & Monitoring | 6-10 hours | API Enhancement |

**Total Estimated Time**: 25-38 hours of development work

---

## 🚨 **BREAKING CHANGES WARNING**

### ⚠️ **API Changes**
- Premium status parameter will be added to all actions
- Some hardcoded premium checks will change behavior
- Banner queue system will change content update patterns

### ⚠️ **Configuration Changes**
- Banner rotation settings will move to VortiDeck platform
- Premium status detection will be more responsive
- Default banner behavior may change

### ⚠️ **Testing Requirements**
- Full regression testing required for premium status changes
- Banner queue system needs extensive testing
- Connection loss scenarios must be validated

---

**💡 TIP**: Start with content type detection improvements and premium status refactor as they provide immediate code quality improvements and enable better implementation of the banner queue system. 


more:
- test videos, implement
- test queques implement
- add/test templatef for diffrent types of adds? (on platform?)
- when premium status is true and customizable sstatus for add then check if the custom_css on vortideck banner is filled
- on sources + menu adding vortideck banner adds something completly else than the actual browser source we use for vortideck, its completly diffrent object. please replace it with correct objet and remove legacy code  connected to the wrong one
- premium users CANT open the properties, the properties button of banner should be grayed out for them.??
-there is no need for auti_rotation we send banners and they cannot directly set things which happens by default. auto rotation should be send diffrently. 


----------
i turned off and on vortideck and i couldnt cal any plugins actions. 20:48:54.490: Sending registration message...
20:48:54.491: Registration message sent
20:48:54.491: Integration registered
20:48:54.493: Received payload: {"path":"/api/v1/integration/activate","payload":{"instanceGuid":"obs_plugin_4ede7f3f-de19-4285-add4-1e1ac4676ccc","integrationGuid":"obs_plugin_8a5f53e4-f72b-4937-854f-170541215471"},"verb":"SET"}
20:48:54.493: Parsing JSON payload...
20:48:54.493: Activation response received
20:48:54.493: Integration activated with GUID: obs_plugin_8a5f53e4-f72b-4937-854f-170541215471
20:48:54.493: Registering regular actions...
20:48:54.493: Registering parameterized actions...
20:48:54.493: Starting status update loop...
20:48:54.493: Actions initialized
20:48:54.493: Received payload: {"action":"server_info","server":{"device_type":"Windows_NT","icons_server_port":9002,"ip":"192.168.178.129","port":9001,"server_name":"DESKTOP-LO42VDR"}}
20:48:54.493: Parsing JSON payload...
20:48:54.494: ERROR: Exception in message handling: [json.exception.type_error.302] type must be string, but is null
20:48:59.494: Initialization timeout no more logs from reciving calls liek websocket was closed or smth..