# Development Guide

## Building the Plugin

### Prerequisites
- **CMake** 3.16+
- **Visual Studio 2019+** (Windows) or **Xcode** (macOS)
- **OBS Studio Development Libraries**
- **Dependencies**: websocketpp, nlohmann/json, asio

### Build Steps

#### Windows
```bash
mkdir build
cd build
cmake .. -G "Visual Studio 16 2019" -A x64
cmake --build . --config Release
```

#### macOS
```bash
mkdir build
cd build
cmake .. -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
make -j4
```

### Dependencies Structure
```
obs_plugin/
├── libs/
│   ├── x64/           # Windows x64 libraries
│   │   ├── obs.lib
│   │   └── obs-frontend-api.lib
│   └── osx/           # macOS libraries
│       ├── libobs.dylib
│       └── obs-frontend-api.dylib
├── src/
│   ├── obs_plugin.cpp
│   └── obs_plugin.hpp
└── CMakeLists.txt
```

## Adding New Actions

### 1. Define Action Constants
```cpp
// In obs_plugin.hpp
namespace actions {
    const std::string s_new_action = "obs_new_action";
}
```

### 2. Add Function Declaration
```cpp
// In obs_plugin.hpp
void action_new_action(const action_invoke_parameters &parameters);
```

### 3. Implement Action Handler
```cpp
// In obs_plugin.cpp
void vorti::applets::obs_plugin::action_new_action(const action_invoke_parameters &parameters)
{
    // Validate parameters
    if (!parameters.empty()) {
        return; // or handle parameters
    }
    
    // Implement action logic
    log_to_obs("New action executed");
}
```

### 4. Register Action
```cpp
// In register_regular_actions() or register_parameter_actions()
actions.push_back(register_action(actions::s_new_action, "APPLET_OBS_NEW_ACTION"));
```

### 5. Add Message Handler
```cpp
// In websocket_message_handler()
else if (action_id == actions::s_new_action)
{
    action_new_action(parameters);
}
```

## Banner System Extension

### Custom Banner Properties
```cpp
// Modify helper_add_banner_to_current_scene()
struct vec2 custom_pos;
vec2_set(&custom_pos, x, y);  // Custom position
obs_sceneitem_set_pos(item, &custom_pos);

struct obs_transform_info transform;
obs_sceneitem_get_info(item, &transform);
transform.bounds_type = OBS_BOUNDS_SCALE_INNER;
transform.bounds.x = width;
transform.bounds.y = height;
obs_sceneitem_set_info(item, &transform);
```

### Multiple Banner Support
```cpp
// Add to header
std::map<std::string, obs_source_t*> m_banner_sources;

// Create named banners
void helper_create_named_banner(std::string_view name);
void helper_set_named_banner_content(std::string_view name, std::string_view content);
```

## WebSocket Protocol Extension

### Custom Message Types
```cpp
// Add new message handlers
void handle_custom_message(const nlohmann::json& message);

// In websocket_message_handler()
if (path == "/api/v1/custom/endpoint") {
    handle_custom_message(message);
}
```

### Status Reporting
```cpp
// Send status updates to server
void send_status_update() {
    nlohmann::json status = {
        {"path", "/api/v1/status/update"},
        {"verb", "SET"},
        {"payload", {
            {"streaming", obs_frontend_streaming_active()},
            {"recording", obs_frontend_recording_active()},
            {"banner_visible", m_banner_visible}
        }}
    };
    send_message(status);
}
```

## Testing

### Unit Tests
```cpp
// Test action handlers
void test_banner_actions() {
    action_invoke_parameters empty_params;
    
    // Test show/hide
    action_banner_show(empty_params);
    assert(m_banner_visible == true);
    
    action_banner_hide(empty_params);
    assert(m_banner_visible == false);
}
```

### Integration Tests
```javascript
// WebSocket test client
const testActions = [
    'obs_stream_start',
    'obs_banner_show',
    'obs_banner_hide'
];

testActions.forEach(action => {
    ws.send(JSON.stringify({
        path: "/api/v1/integration/sdk/action/invoke",
        verb: "BROADCAST",
        payload: { actionId: action, parameters: {} }
    }));
});
```

## Debugging

### Enable Debug Logging
```cpp
#define DEBUG_LOGGING 1

void debug_log(std::string_view message) {
#ifdef DEBUG_LOGGING
    log_to_obs("[DEBUG] " + message);
#endif
}
```

### Memory Debugging
```cpp
void log_memory_usage() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        log_to_obs("Memory usage: " + std::to_string(pmc.WorkingSetSize / 1024 / 1024) + " MB");
    }
}
```

## Code Style

- Follow existing naming conventions
- Use `log_to_obs()` for all logging
- Lock shared resources with mutexes
- Handle errors gracefully without crashing OBS
- Document public functions

## Contributing

1. Fork the repository
2. Create feature branch
3. Test thoroughly with OBS
4. Submit pull request with clear description
5. Ensure backward compatibility
``` 