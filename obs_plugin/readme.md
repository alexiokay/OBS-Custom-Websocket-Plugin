# VortiDeck OBS Plugin Documentation

The VortiDeck OBS Plugin enables remote control of OBS Studio through WebSocket connections, with special focus on banner management and streaming controls.

## Features

### Core OBS Controls
- **Streaming Control**: Start, stop, toggle streaming
- **Recording Control**: Start, stop, toggle recording  
- **Buffer Control**: Start, stop, toggle, save replay buffer
- **Audio Control**: Mute/unmute desktop audio and microphone
- **Scene Management**: Switch between scenes and collections
- **Source Control**: Show/hide sources and mixers

### Banner System
- **Dynamic Banner Display**: Overlay banners on scenes that cannot be disabled by users
- **Real-time Content Updates**: Send images/videos directly through WebSocket
- **Multiple Content Types**: Support for PNG, JPG, GIF, MP4, and URLs
- **Scene Integration**: Automatically adds banner to all scenes as locked overlay
- **Menu Integration**: OBS Tools menu for manual banner control

## Quick Start

1. **Install the Plugin**: Copy the compiled plugin to your OBS plugins directory
2. **Start OBS**: The plugin automatically connects to WebSocket server on port 9001
3. **Connect Your Server**: Use the WebSocket API to send commands
4. **Control Banners**: Send banner content and visibility commands

## Documentation Structure

- [Installation Guide](installation.md) - How to install and configure the plugin
- [API Reference](api-reference.md) - Complete WebSocket API documentation
- [Banner System](banner-system.md) - Banner functionality and usage
- [Examples](examples.md) - Code examples and use cases
- [Troubleshooting](troubleshooting.md) - Common issues and solutions
- [Development](development.md) - Building and extending the plugin

## WebSocket Connection

The plugin connects to `ws://localhost:9001` by default and uses JSON messages for communication.

### Basic Message Format
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "SET",
  "payload": {
    "actionId": "action_name",
    "parameters": {}
  }
}
```

## Quick Examples

### Show Banner
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "SET",
  "payload": {
    "actionId": "obs_banner_show",
    "parameters": {}
  }
}
```

### Set Banner Content (Base64)
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "SET",
  "payload": {
    "actionId": "obs_banner_set_data",
    "parameters": {
      "content_data": "base64_encoded_image_data",
      "content_type": "image/png"
    }
  }
}
```

### Start Streaming
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "SET", 
  "payload": {
    "actionId": "obs_stream_start",
    "parameters": {}
  }
}
```

## Support

For issues and questions, please refer to the [Troubleshooting Guide](troubleshooting.md) or contact the VortiDeck team.