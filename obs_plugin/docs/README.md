# VortiDeck OBS Plugin Documentation

The VortiDeck OBS Plugin enables remote control of OBS Studio through WebSocket connections, with special focus on banner management and streaming controls.

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Documentation](#documentation)
- [Development](#development)
- [Support](#support)

## Overview

VortiDeck OBS Plugin creates a bridge between OBS Studio and the VortiDeck desktop application, enabling remote control of streaming, recording, scenes, and displaying dynamic banners that cannot be disabled by users.

### Key Components

- **WebSocket Server**: Connects to VortiDeck app on `ws://localhost:9001`
- **Action System**: Exposes OBS functions as remotely invocable actions
- **Banner System**: Dynamic overlay system for real-time content display
- **Menu Integration**: Tools menu for manual banner control

## Features

### Core OBS Controls
- **Streaming Control**: Start, stop, toggle streaming
- **Recording Control**: Start, stop, toggle recording  
- **Buffer Control**: Start, stop, toggle, save replay buffer
- **Audio Control**: Mute/unmute desktop audio and microphone
- **Scene Management**: Switch between scenes and collections
- **Source Control**: Show/hide sources and mixers

### Banner System
- **Dynamic Banner Display**: Overlay banners on scenes that cannot be disabled
- **Real-time Updates**: Content changes via WebSocket commands
- **Multiple Formats**: Support for images (PNG, JPG, GIF) and videos (MP4, MOV)
- **Base64 Content**: Direct content transmission without file dependencies
- **Persistent Overlay**: Banner appears on all scenes automatically

### Menu Integration
- **Tools Menu**: "VortiDeck Banner" submenu in OBS Tools menu
- **Manual Control**: Show/hide banner manually
- **Settings Access**: Quick access to banner configuration

## Installation

See [Installation Guide](installation.md) for detailed setup instructions.

### Quick Install
1. Download the plugin from releases
2. Copy to OBS plugins directory
3. Restart OBS Studio
4. Connect VortiDeck desktop application

## Quick Start

### 1. Basic Connection
Once installed, the plugin automatically connects to VortiDeck on startup:
- Plugin connects to `ws://localhost:9001`
- Registers available actions with VortiDeck
- Ready for remote control

### 2. Banner Control
Control banners via WebSocket:

```json
// Show banner
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "BROADCAST",
  "payload": {
    "actionId": "obs_banner_show"
  }
}

// Set banner content
{
  "path": "/api/v1/integration/sdk/action/invoke", 
  "verb": "BROADCAST",
  "payload": {
    "actionId": "obs_banner_set_data",
    "parameters": {
      "content_data": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAY...",
      "content_type": "image/png"
    }
  }
}
```

### 3. OBS Controls
Stream and record remotely:

```json
// Start streaming
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "BROADCAST", 
  "payload": {
    "actionId": "obs_stream_start"
  }
}
```

## Documentation

- **[API Reference](api-reference.md)**: Complete WebSocket API documentation
- **[Banner System](banner-system.md)**: Detailed banner functionality guide
- **[Examples](examples.md)**: Usage examples and code samples
- **[Installation](installation.md)**: Setup and configuration guide
- **[Development](development.md)**: Building and extending the plugin
- **[Troubleshooting](troubleshooting.md)**: Common issues and solutions

## Development

### Building the Plugin
```bash
# Clone the repository
git clone <repository-url>
cd obs-plugin

# Build with CMake
mkdir build
cd build
cmake ..
make
```

### Adding New Actions
1. Add action ID to `actions` namespace in header
2. Add action handler function declaration
3. Implement action handler in source file
4. Register action in `register_regular_actions()`
5. Add to websocket message handler

See [Development Guide](development.md) for detailed instructions.

### Banner System Extension
The banner system can be extended to support:
- Custom positioning and scaling
- Animation effects
- Multiple banner layers
- User-defined styles

## Support

### Logging
Plugin logs all activities to OBS log files:
- **Windows**: `%APPDATA%\obs-studio\logs\`
- **macOS**: `~/Library/Application Support/obs-studio/logs/`

### Common Issues
- **Connection Failed**: Ensure VortiDeck app is running on port 9001
- **Banner Not Visible**: Check if banner source exists in current scene
- **Actions Not Working**: Verify WebSocket connection and registration

See [Troubleshooting Guide](troubleshooting.md) for detailed solutions.

### Getting Help
- Check the documentation in this `docs/` directory
- Review OBS Studio logs for error messages
- Ensure VortiDeck application compatibility

## License

This plugin is part of the VortiDeck ecosystem.

## Version Information

- **Plugin Version**: 1.0.0
- **OBS Studio Compatibility**: 28.0+
- **VortiDeck Integration**: API v1
- **WebSocket Protocol**: JSON over WebSocket 