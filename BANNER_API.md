# VortiDeck OBS Banner Plugin API Documentation

## Overview

The VortiDeck OBS Banner Plugin extends the existing OBS plugin with WebSocket-controlled banner functionality. This allows external applications to dynamically control banner content and visibility in OBS scenes.

## Features Implemented

### 1. **OBS Menu Integration**
- **Feature**: Adds "Toggle VortiDeck Banner" to the OBS Tools menu
- **Implementation**: `obs_frontend_add_tools_menu_item()` in `add_banner_menu()`
- **Functionality**: Manual toggle of banner visibility

### 2. **WebSocket Server**
- **Port**: `ws://localhost:9001`
- **Protocol**: JSON message-based commands
- **Implementation**: WebSocket++ server with async message handling
- **Auto-start**: Server starts automatically when plugin loads

### 3. **Dynamic Banner Source Management**
- **Source Name**: `vortideck_banner`
- **Supported Formats**: 
  - **Images**: PNG, JPG, JPEG, BMP, GIF, TGA, WEBP
  - **Videos**: MP4, AVI, MOV, MKV, FLV, WMV, WEBM, M4V
- **Source Types**: 
  - `image_source` for static images
  - `ffmpeg_source` for videos (with looping enabled)

### 4. **Visibility Control**
- **Show/Hide**: Toggle banner visibility across all scenes
- **Scene Integration**: Automatically adds to current scene when shown
- **Persistent**: Banner remains in scenes until explicitly hidden

### 5. **Dynamic Content Management**
- **Runtime Updates**: Change banner content without restarting OBS
- **File Validation**: Checks file existence and format support
- **Automatic Refresh**: Updates all scenes when content changes

### 6. **Locked Scene Items**
- **Implementation**: `obs_sceneitem_set_locked(item, true)`
- **Purpose**: Prevents accidental modification/deletion in OBS UI
- **Scope**: All banner items are automatically locked when added

## API Commands

### WebSocket Message Format
```json
{
  "command": "command_name",
  "parameter_name": "parameter_value"
}
```

### Available Commands

#### 1. Show Banner
```json
{
  "command": "show_banner"
}
```
- **Action**: Makes the banner visible in the current scene
- **Behavior**: Creates banner source if not set, adds to scene if not present

#### 2. Hide Banner
```json
{
  "command": "hide_banner"
}
```
- **Action**: Hides the banner from all scenes
- **Behavior**: Sets visibility to false without removing from scenes

#### 3. Set Banner Content
```json
{
  "command": "set_banner",
  "file_path": "/path/to/banner/file.png"
}
```
- **Action**: Changes the banner content to specified file
- **Parameters**: 
  - `file_path`: Full path to image or video file
- **Validation**: Checks file existence and format support
- **Auto-update**: Refreshes scenes if banner is currently visible

## Technical Implementation

### Class Structure
```cpp
namespace vorti::applets::obs_plugin {
    // WebSocket server components
    ws_server m_banner_server;
    std::thread m_banner_server_thread;
    std::atomic<bool> m_banner_server_running;
    
    // Banner management
    obs_source_t* m_banner_source;
    std::string m_current_banner_file;
    bool m_banner_visible;
    
    // Connection management
    std::set<websocketpp::connection_hdl> m_banner_connections;
    std::mutex m_banner_connections_mutex;
}
```

### Key Functions

#### Server Management
- `start_banner_server()`: Initialize and start WebSocket server
- `stop_banner_server()`: Clean shutdown of server
- `handle_banner_message()`: Parse and route incoming commands

#### Banner Control
- `show_banner()`: Make banner visible in current scene
- `hide_banner()`: Hide banner from all scenes
- `set_banner_content()`: Update banner content
- `create_banner_source()`: Create appropriate OBS source for file type

#### Scene Management
- `add_banner_to_current_scene()`: Add banner to active scene
- `remove_banner_from_scenes()`: Hide banner from all scenes
- `lock_banner_item()`: Lock scene item to prevent modification

#### Utility Functions
- `is_image_file()`: Check if file is supported image format
- `is_video_file()`: Check if file is supported video format

## Usage Examples

### Python WebSocket Client
```python
import asyncio
import websockets
import json

async def control_banner():
    uri = "ws://localhost:9001"
    async with websockets.connect(uri) as websocket:
        # Set banner content
        await websocket.send(json.dumps({
            "command": "set_banner",
            "file_path": "C:/path/to/banner.png"
        }))
        
        # Show banner
        await websocket.send(json.dumps({
            "command": "show_banner"
        }))
        
        # Hide after 5 seconds
        await asyncio.sleep(5)
        await websocket.send(json.dumps({
            "command": "hide_banner"
        }))

asyncio.run(control_banner())
```

### JavaScript WebSocket Client
```javascript
const ws = new WebSocket('ws://localhost:9001');

ws.onopen = function() {
    // Set banner content
    ws.send(JSON.stringify({
        command: 'set_banner',
        file_path: 'C:/path/to/banner.mp4'
    }));
    
    // Show banner
    ws.send(JSON.stringify({
        command: 'show_banner'
    }));
};

ws.onmessage = function(event) {
    console.log('Response:', event.data);
};
```

## Integration with Existing Plugin

### Port Configuration
- **Original Plugin**: Port 9002 (changed from 9001 to avoid conflict)
- **Banner Server**: Port 9001
- **Coexistence**: Both servers run independently

### VortiDeck Actions Integration
The banner functionality is also available through the existing VortiDeck action system:
- `obs_banner_show`
- `obs_banner_hide` 
- `obs_banner_toggle`
- `obs_banner_set_content` (with file_path parameter)

## Error Handling

### File Validation
- Checks file existence before setting content
- Validates file format against supported types
- Logs errors to OBS log system

### WebSocket Error Handling
- Graceful connection management
- Exception handling for malformed JSON
- Automatic cleanup on client disconnect

### Resource Management
- Proper OBS source reference counting
- Thread-safe server operations
- Memory cleanup on plugin unload

## Logging

All banner operations are logged to the OBS log system with the prefix identifying the source:
```
[vorti_obs_plugin] Banner WebSocket server started on port 9001
[vorti_obs_plugin] Banner shown
[vorti_obs_plugin] Banner content set to: /path/to/file.png
```

## Installation and Build

The banner functionality is integrated into the existing plugin build system. No additional dependencies are required beyond what's already included:
- WebSocket++ (already included)
- nlohmann/json (already included)
- Standard OBS development libraries

## Security Considerations

### Local Server Only
- WebSocket server binds to localhost only
- No external network access by default
- Suitable for local control applications

### File System Access
- Plugin validates file paths
- No arbitrary code execution
- File format validation prevents malicious content

### Resource Limits
- Single banner source per plugin instance
- Limited connection pool for WebSocket clients
- Automatic cleanup prevents resource leaks