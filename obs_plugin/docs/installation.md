# Installation Guide

## Requirements

- **OBS Studio**: Version 28.0 or later
- **Operating System**: Windows 10/11 (x64) or macOS 12.0+
- **WebSocket Server**: Running on port 9001
- **Dependencies**: Visual C++ Redistributable (Windows) or Xcode Command Line Tools (macOS)

## Installation Steps

### Windows

1. **Download Plugin**
   - Get `vorti_obs_plugin_x64.dll` from releases
   
2. **Install to OBS**
   ```
   Copy to: C:\Program Files\obs-studio\obs-plugins\64bit\
   ```

3. **Restart OBS Studio**

### macOS

1. **Download Plugin**
   - Get `vorti_obs.plugin` bundle from releases
   
2. **Install to OBS**
   ```
   Copy to: /Applications/OBS.app/Contents/PlugIns/
   ```

3. **Restart OBS Studio**

## Verification

### Check Plugin Loading
- Look for "VortiDeck Banner" in OBS Tools menu
- Check OBS logs for VortiDeck entries
- Verify WebSocket connection attempts in logs

### Test Connection
1. Start WebSocket server on port 9001
2. Start OBS Studio
3. Check logs for "Connection established" message
4. Use Tools → VortiDeck Banner to test functionality

## WebSocket Server Requirements

Your WebSocket server must:
- Listen on `localhost:9001`
- Handle JSON message format
- Support the integration registration flow
- Send BROADCAST messages for action invocation

## Troubleshooting

### Common Issues
- **Plugin not loading**: Check file permissions and dependencies
- **Connection failed**: Verify WebSocket server is running on port 9001
- **Banner not appearing**: Check OBS logs for banner creation errors

See [Troubleshooting Guide](troubleshooting.md) for detailed solutions.
