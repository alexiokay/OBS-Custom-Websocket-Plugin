# Troubleshooting Guide

## Installation Issues

### Plugin Not Loading
**Symptoms:** No VortiDeck menu, no log entries
**Solutions:**
- Verify correct file location and permissions
- Install Visual C++ Redistributable (Windows) or Xcode tools (macOS)
- Check antivirus/security software blocking
- Ensure OBS Studio 28.0+

### Connection Issues
**Symptoms:** "Connection timeout", "Failed to connect"
**Solutions:**
- Start WebSocket server before OBS
- Check port 9001 availability: `netstat -an | findstr :9001`
- Verify firewall settings
- Test server with: `curl -i -N -H "Connection: Upgrade" -H "Upgrade: websocket" http://localhost:9001`

## Banner Issues

### Banner Not Visible
**Check:**
- Banner source exists in OBS sources
- Banner is added to current scene
- Banner has valid content loaded
- Banner visibility is enabled

### Content Not Loading
**Solutions:**
- Verify Base64 encoding is valid
- Check file format support (PNG, JPG, GIF, MP4)
- Reduce content size if memory issues
- Check OBS logs for loading errors

## Performance Issues

### High Memory Usage
**Solutions:**
- Reduce banner image resolution
- Clean temporary files: `%TEMP%\vortideck_banners\`
- Monitor OBS memory usage
- Limit banner update frequency

### Slow Response
**Solutions:**
- Reduce WebSocket message frequency
- Check network latency
- Monitor OBS performance impact
- Optimize banner content size

## Debugging

### Enable Detailed Logging
Plugin logs appear in OBS log files with "VortiDeck" prefix.

### Test WebSocket Connection
```javascript
const ws = new WebSocket('ws://localhost:9001');
ws.onopen = () => console.log('Connected');
ws.onerror = (error) => console.error('Error:', error);
```

### Manual Banner Control
Use OBS Tools → VortiDeck Banner for manual testing.

## Getting Help

1. Check OBS log files for error details
2. Verify WebSocket server implementation
3. Test with minimal banner content
4. Review [API Reference](api-reference.md) for correct message format 