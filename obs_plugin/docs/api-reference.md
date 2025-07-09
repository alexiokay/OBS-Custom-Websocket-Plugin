# API Reference

Complete WebSocket API documentation for the VortiDeck OBS Plugin.

## Connection

- **URL**: `ws://localhost:9001`
- **Protocol**: WebSocket with JSON messages
- **Subprotocol**: `json`

## Message Templates

### Plugin Registration (Plugin → Server)
```json
{
  "path": "/api/v1/integration/register",
  "verb": "SET",
  "payload": {
    "integrationIdentifier": "vortideck_obs_plugin",
    "name": "APPLET_OBS_NAME",
    "author": "VortiDeck",
    "description": "A VortiDeck plugin for Open Broadcaster Software, exposing additional actions to VortiDeck desktop app.",
    "icon": "base64_encoded_icon_data",
    "manualRegistration": true
  }
}
```

### Integration Activation (Plugin → Server)
```json
{
  "path": "/api/v1/integration/activate",
  "verb": "SET",
  "payload": {
    "integrationIdentifier": "vortideck_obs_plugin",
    "sdkType": "ACTION"
  }
}
```

### Action Registration (Plugin → Server)
```json
{
  "path": "/api/v1/actions/register",
  "verb": "SET",
  "payload": {
    "actions": [/* array of action objects */],
    "instance": {
      "integrationGuid": "provided_by_server",
      "instanceGuid": "provided_by_server"
    }
  }
}
```

### Action Subscription (Plugin → Server)
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "SUBSCRIBE"
}
```

### Action Invocation (Server → Plugin)
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "BROADCAST",
  "payload": {
    "actionId": "action_name",
    "integrationGuid": "provided_by_server",
    "parameters": {
      "param1": "value1",
      "param2": "value2"
    }
  }
}
```

## Available Actions

### Streaming Controls
| Action ID | Description | Parameters |
|-----------|-------------|------------|
| `obs_stream_start` | Start streaming | None |
| `obs_stream_stop` | Stop streaming | None |
| `obs_stream_toggle` | Toggle streaming | None |

### Recording Controls
| Action ID | Description | Parameters |
|-----------|-------------|------------|
| `obs_recording_start` | Start recording | None |
| `obs_recording_stop` | Stop recording | None |
| `obs_recording_toggle` | Toggle recording | None |

### Replay Buffer Controls
| Action ID | Description | Parameters |
|-----------|-------------|------------|
| `obs_buffer_start` | Start replay buffer | None |
| `obs_buffer_stop` | Stop replay buffer | None |
| `obs_buffer_toggle` | Toggle replay buffer | None |
| `obs_buffer_save` | Save replay buffer | None |

### Audio Controls
| Action ID | Description | Parameters |
|-----------|-------------|------------|
| `obs_desktop_mute` | Mute desktop audio | None |
| `obs_desktop_unmute` | Unmute desktop audio | None |
| `obs_desktop_mute_toggle` | Toggle desktop audio mute | None |
| `obs_mic_mute` | Mute microphone | None |
| `obs_mic_unmute` | Unmute microphone | None |
| `obs_mic_mute_toggle` | Toggle microphone mute | None |

### Banner Controls
| Action ID | Description | Parameters |
|-----------|-------------|------------|
| `obs_banner_show` | Show banner | None |
| `obs_banner_hide` | Hide banner | None |
| `obs_banner_toggle` | Toggle banner visibility | None |
| `obs_banner_set_data` | Set banner from base64 data | `content_data` (string), `content_type` (string) |

### Scene Management
| Action ID | Description | Parameters |
|-----------|-------------|------------|
| `obs_scene_activate` | Switch to scene | `scene_name` (string) |
| `obs_collection_activate` | Switch to scene collection | `collection_name` (string) |

### Source Controls
| Action ID | Description | Parameters |
|-----------|-------------|------------|
| `obs_source_activate` | Show source | `source_name` (string) |
| `obs_source_deactivate` | Hide source | `source_name` (string) |
| `obs_source_toggle` | Toggle source visibility | `source_name` (string) |

### Mixer Controls
| Action ID | Description | Parameters |
|-----------|-------------|------------|
| `obs_mixer_mute` | Mute mixer | `mixer_name` (string) |
| `obs_mixer_unmute` | Unmute mixer | `mixer_name` (string) |
| `obs_mixer_mute_toggle` | Toggle mixer mute | `mixer_name` (string) |

## Parameter Details

### Banner Parameters
- **`content_data`**: Base64 encoded image/video data
- **`content_type`**: MIME type (`image/png`, `image/jpeg`, `image/gif`, `video/mp4`, etc.)

### Scene/Source Parameters
- **`scene_name`**: Exact name of the scene in OBS
- **`collection_name`**: Exact name of the scene collection in OBS
- **`source_name`**: Exact name of the source in OBS
- **`mixer_name`**: Exact name of the audio mixer in OBS

## Example Usage

### Start Streaming
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "BROADCAST",
  "payload": {
    "actionId": "obs_stream_start",
    "integrationGuid": "your_integration_guid",
    "parameters": {}
  }
}
```

### Set Banner with Image
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "BROADCAST",
  "payload": {
    "actionId": "obs_banner_set_data",
    "integrationGuid": "your_integration_guid",
    "parameters": {
      "content_data": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAY...",
      "content_type": "image/png"
    }
  }
}
```

### Switch Scene
```json
{
  "path": "/api/v1/integration/sdk/action/invoke",
  "verb": "BROADCAST",
  "payload": {
    "actionId": "obs_scene_activate",
    "integrationGuid": "your_integration_guid",
    "parameters": {
      "scene_name": "Gaming Scene"
    }
  }
}
```

## Error Handling

- Plugin logs all errors to OBS log files
- Failed actions are logged with specific error messages
- Connection issues trigger automatic reconnection attempts
- Banner operations fail gracefully without affecting OBS performance

## Notes

- The plugin uses the **BROADCAST** verb for all action invocations
- All actions require the correct `integrationGuid` provided during registration
- Banner content is sent as base64 encoded data directly through WebSocket
- Menu items appear in OBS Tools menu for manual banner control