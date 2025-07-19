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

## Development Lessons Learned

### Critical Architecture Understanding

#### Qt Threading in OBS Plugins
**⚠️ CRITICAL:** Always understand thread context when working with Qt dialogs in OBS plugins.

```cpp
// ✅ CORRECT: Check thread before using QMetaObject::invokeMethod
bool on_main_thread = (QThread::currentThread() == app->thread());
if (on_main_thread) {
    // Call dialog directly - we're already on main thread
    dialog.exec();
} else {
    // Use QMetaObject::invokeMethod for cross-thread calls
    QMetaObject::invokeMethod(app, [&]() {
        dialog.exec();
    }, Qt::BlockingQueuedConnection);
}
```

**❌ COMMON MISTAKE:** Using `Qt::BlockingQueuedConnection` from main thread to main thread causes deadlocks:
```cpp
// This will deadlock if called from main thread!
QMetaObject::invokeMethod(app, [&]() {
    dialog.exec();
}, Qt::BlockingQueuedConnection);
```

#### OBS Menu Integration Pattern
```cpp
// ✅ CORRECT Pattern for OBS menu callbacks
class MyPlugin {
    static void menu_callback(void* data) {
        if (data) {
            auto* plugin = static_cast<MyPlugin*>(data);
            plugin->show_settings_dialog();
        }
    }
    
    void add_menu() {
        obs_frontend_add_tools_menu_item("My Settings", menu_callback, this);
    }
};
```

### Common Threading Mistakes to Avoid

1. **Nested Qt Threading:**
   ```cpp
   // ❌ DON'T: Nest QMetaObject::invokeMethod calls
   QMetaObject::invokeMethod(app, []() {
       some_function_that_also_uses_invokeMethod(); // DEADLOCK!
   });
   ```

2. **Assuming Thread Context:**
   ```cpp
   // ❌ DON'T: Assume you know which thread you're on
   dialog.exec(); // May crash if not on main thread
   
   // ✅ DO: Always verify thread context
   if (QThread::currentThread() == qApp->thread()) {
       dialog.exec();
   } else {
       // Handle cross-thread case
   }
   ```

### Architecture Discovery Process

#### Before Adding New Features:
1. **Map the Architecture First**
   - Identify if functions are namespace-level or class methods
   - Find global instances vs local state management
   - Understand thread boundaries and data flow

2. **Study Working Examples**
   - Look for similar features already implemented
   - Follow established patterns (like `banner_manager` menu integration)
   - Reuse existing functions rather than rewriting

3. **Check Existing Error Handling**
   - Most mature functions already handle edge cases
   - Don't duplicate error handling logic
   - Add minimal glue code between existing functions

### Development Anti-Patterns Learned

#### ❌ DON'T: Reinvent Existing Functionality
```cpp
// Bad: Duplicating service discovery logic
void menu_callback() {
    // 50 lines of discovery logic copied from elsewhere...
}
```

#### ✅ DO: Reuse and Orchestrate
```cpp
// Good: Orchestrate existing functions
void menu_callback() {
    m_show_selection_dialog = false;  // Reset state
    show_service_selection_dialog();  // Reuse existing function
}
```

#### ❌ DON'T: Ignore Compilation Errors
Compilation errors often reveal architectural misunderstandings:
- `'this' can only be used as a lambda capture...` → You're in wrong scope
- `identifier not found` → Missing dependencies/includes
- `use of undefined type` → Missing forward declarations

#### ✅ DO: Learn from Build Errors
Each error teaches you about the codebase structure.

### Best Practices Discovered

#### 1. Thread-Safe Dialog Pattern
```cpp
class DialogManager {
    void show_dialog_thread_safe() {
        QApplication* app = qApp;
        if (!app) return;
        
        bool on_main_thread = (QThread::currentThread() == app->thread());
        
        if (on_main_thread) {
            create_and_show_dialog();
        } else {
            QMetaObject::invokeMethod(app, [this]() {
                create_and_show_dialog();
            }, Qt::QueuedConnection);
        }
    }
};
```

#### 2. State Reset for Manual Triggers
```cpp
void manual_action_trigger() {
    // Always reset flags that prevent automatic dialogs
    m_show_selection_dialog = false;
    m_connection_failure_count = 0;
    
    // Then trigger the action
    perform_action();
}
```

#### 3. Defensive Logging During Development
```cpp
void complex_function() {
    log_to_obs("DEBUG: Function started");
    log_to_obs(std::format("DEBUG: Thread: {}", 
        QThread::currentThread() == qApp->thread() ? "MAIN" : "BACKGROUND"));
    
    // ... implementation ...
    
    log_to_obs("DEBUG: Function completed successfully");
}
```

### Process Improvements

#### Development Workflow:
1. **Architecture Mapping** → Draw components and data flow first
2. **Pattern Study** → Find similar working features
3. **Incremental Implementation** → Small changes, test each step
4. **Thread Safety Review** → Consider all threading implications
5. **Integration Testing** → Test in actual OBS environment

#### Code Review Checklist:
- [ ] No nested `QMetaObject::invokeMethod` calls
- [ ] Thread context verified before Qt operations
- [ ] Existing patterns followed consistently
- [ ] State properly reset for manual triggers
- [ ] Error cases handled gracefully
- [ ] Logging added for debugging complex flows

### Debugging Techniques

#### Qt Threading Issues:
```cpp
// Add this to identify threading problems
void log_thread_context(const std::string& location) {
    bool on_main = (QThread::currentThread() == qApp->thread());
    log_to_obs(std::format("THREAD CHECK [{}]: {}", 
        location, on_main ? "MAIN" : "BACKGROUND"));
}
```

#### Deadlock Detection:
- Look for "QMetaObject::invokeMethod: Dead lock detected" in logs
- Check for blocking calls from main thread to main thread
- Verify Qt connection types match thread context

## Contributing

1. Fork the repository
2. Create feature branch
3. **Review architecture patterns above** before implementing
4. Test thoroughly with OBS (especially threading scenarios)
5. Submit pull request with clear description
6. Ensure backward compatibility
``` 