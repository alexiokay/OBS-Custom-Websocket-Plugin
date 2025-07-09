# Examples

Practical examples and code samples for using the VortiDeck OBS Plugin.

## WebSocket Server Examples

### Node.js Server

```javascript
const WebSocket = require('ws');
const fs = require('fs');

const wss = new WebSocket.Server({ port: 9001 });

wss.on('connection', function connection(ws) {
  console.log('OBS Plugin connected');

  // Example: Show banner with image
  function showBannerWithImage(imagePath) {
    const imageData = fs.readFileSync(imagePath);
    const base64Data = imageData.toString('base64');
    
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_set_data",
        parameters: {
          content_data: base64Data,
          content_type: "image/png"
        }
      }
    };
    
    ws.send(JSON.stringify(message));
    
    // Show the banner
    const showMessage = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_show",
        parameters: {}
      }
    };
    
    ws.send(JSON.stringify(showMessage));
  }

  // Example: Start streaming
  function startStreaming() {
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_stream_start",
        parameters: {}
      }
    };
    
    ws.send(JSON.stringify(message));
  }

  // Handle incoming messages from plugin
  ws.on('message', function incoming(data) {
    try {
      const message = JSON.parse(data);
      console.log('Received:', message);
    } catch (error) {
      console.error('Invalid JSON:', error);
    }
  });

  // Example usage
  setTimeout(() => {
    showBannerWithImage('./banner.png');
  }, 2000);
});
```

### Python Server

```python
import asyncio
import websockets
import json
import base64

class OBSController:
    def __init__(self):
        self.websocket = None
    
    async def send_command(self, action_id, parameters=None):
        if not self.websocket:
            return False
            
        message = {
            "path": "/api/v1/integration/sdk/action/invoke",
            "verb": "SET",
            "payload": {
                "actionId": action_id,
                "parameters": parameters or {}
            }
        }
        
        await self.websocket.send(json.dumps(message))
        return True
    
    async def set_banner_from_file(self, file_path, content_type="image/png"):
        with open(file_path, 'rb') as f:
            file_data = f.read()
            base64_data = base64.b64encode(file_data).decode('utf-8')
        
        await self.send_command("obs_banner_set_data", {
            "content_data": base64_data,
            "content_type": content_type
        })
    
    async def set_banner_from_url(self, url):
        await self.send_command("obs_banner_set_url", {
            "content_url": url
        })
    
    async def show_banner(self):
        await self.send_command("obs_banner_show")
    
    async def hide_banner(self):
        await self.send_command("obs_banner_hide")
    
    async def start_streaming(self):
        await self.send_command("obs_stream_start")
    
    async def stop_streaming(self):
        await self.send_command("obs_stream_stop")

async def handle_client(websocket, path):
    controller = OBSController()
    controller.websocket = websocket
    
    print("OBS Plugin connected")
    
    try:
        # Example: Set banner and show it
        await controller.set_banner_from_file("banner.png")
        await controller.show_banner()
        
        # Keep connection alive
        async for message in websocket:
            data = json.loads(message)
            print(f"Received: {data}")
            
    except websockets.exceptions.ConnectionClosed:
        print("OBS Plugin disconnected")

# Start server
start_server = websockets.serve(handle_client, "localhost", 9001)
asyncio.get_event_loop().run_until_complete(start_server)
asyncio.get_event_loop().run_forever()
```

## Use Cases

### 1. Live Stream Alerts

Display subscriber notifications, donations, etc.

```javascript
// Alert system
class StreamAlerts {
  constructor(websocket) {
    this.ws = websocket;
  }
  
  async showSubscriberAlert(username) {
    // Create alert image with text
    const alertImage = await this.createAlertImage(
      `New Subscriber: ${username}!`,
      'subscriber'
    );
    
    // Send to banner
    await this.sendBannerData(alertImage, 'image/png');
    await this.showBanner();
    
    // Hide after 5 seconds
    setTimeout(() => {
      this.hideBanner();
    }, 5000);
  }
  
  async showDonationAlert(amount, donor) {
    const alertImage = await this.createAlertImage(
      `${donor} donated $${amount}!`,
      'donation'
    );
    
    await this.sendBannerData(alertImage, 'image/png');
    await this.showBanner();
    
    setTimeout(() => {
      this.hideBanner();
    }, 7000);
  }
  
  async sendBannerData(imageBuffer, contentType) {
    const base64Data = imageBuffer.toString('base64');
    
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_set_data",
        parameters: {
          content_data: base64Data,
          content_type: contentType
        }
      }
    };
    
    this.ws.send(JSON.stringify(message));
  }
  
  async showBanner() {
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_show",
        parameters: {}
      }
    };
    
    this.ws.send(JSON.stringify(message));
  }
  
  async hideBanner() {
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_hide",
        parameters: {}
      }
    };
    
    this.ws.send(JSON.stringify(message));
  }
}
```

### 2. Remote Streaming Control

Control OBS from a web dashboard.

```html
<!DOCTYPE html>
<html>
<head>
    <title>OBS Remote Control</title>
    <style>
        .control-panel { padding: 20px; }
        .button { margin: 10px; padding: 10px 20px; }
        .status { margin: 10px 0; }
    </style>
</head>
<body>
    <div class="control-panel">
        <h1>OBS Remote Control</h1>
        
        <div class="status">
            Status: <span id="status">Disconnected</span>
        </div>
        
        <h2>Streaming</h2>
        <button class="button" onclick="startStream()">Start Stream</button>
        <button class="button" onclick="stopStream()">Stop Stream</button>
        <button class="button" onclick="toggleStream()">Toggle Stream</button>
        
        <h2>Banner Control</h2>
        <input type="file" id="bannerFile" accept="image/*">
        <button class="button" onclick="uploadBanner()">Upload Banner</button>
        <button class="button" onclick="showBanner()">Show Banner</button>
        <button class="button" onclick="hideBanner()">Hide Banner</button>
        
        <h2>Audio</h2>
        <button class="button" onclick="toggleMic()">Toggle Mic</button>
        <button class="button" onclick="toggleDesktop()">Toggle Desktop Audio</button>
    </div>

    <script>
        let ws = null;
        
        function connect() {
            ws = new WebSocket('ws://localhost:9001');
            
            ws.onopen = function() {
                document.getElementById('status').textContent = 'Connected';
            };
            
            ws.onclose = function() {
                document.getElementById('status').textContent = 'Disconnected';
                // Reconnect after 3 seconds
                setTimeout(connect, 3000);
            };
            
            ws.onmessage = function(event) {
                console.log('Received:', event.data);
            };
        }
        
        function sendCommand(actionId, parameters = {}) {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                alert('Not connected to OBS');
                return;
            }
            
            const message = {
                path: "/api/v1/integration/sdk/action/invoke",
                verb: "SET",
                payload: {
                    actionId: actionId,
                    parameters: parameters
                }
            };
            
            ws.send(JSON.stringify(message));
        }
        
        function startStream() {
            sendCommand('obs_stream_start');
        }
        
        function stopStream() {
            sendCommand('obs_stream_stop');
        }
        
        function toggleStream() {
            sendCommand('obs_stream_toggle');
        }
        
        function showBanner() {
            sendCommand('obs_banner_show');
        }
        
        function hideBanner() {
            sendCommand('obs_banner_hide');
        }
        
        function toggleMic() {
            sendCommand('obs_mic_mute_toggle');
        }
        
        function toggleDesktop() {
            sendCommand('obs_desktop_mute_toggle');
        }
        
        function uploadBanner() {
            const fileInput = document.getElementById('bannerFile');
            const file = fileInput.files[0];
            
            if (!file) {
                alert('Please select a file');
                return;
            }
            
            const reader = new FileReader();
            reader.onload = function(e) {
                const base64Data = e.target.result.split(',')[1]; // Remove data:image/png;base64,
                
                sendCommand('obs_banner_set_data', {
                    content_data: base64Data,
                    content_type: file.type
                });
            };
            
            reader.readAsDataURL(file);
        }
        
        // Connect on page load
        connect();
    </script>
</body>
</html>
```

### 3. Game Integration

Show game events as banners.

```python
import asyncio
import websockets
import json
import base64
from PIL import Image, ImageDraw, ImageFont
import io

class GameEventBanner:
    def __init__(self, websocket):
        self.ws = websocket
        self.font_path = "arial.ttf"  # Adjust path as needed
    
    def create_banner_image(self, text, background_color=(0, 100, 200), text_color=(255, 255, 255)):
        # Create banner image
        width, height = 800, 100
        image = Image.new('RGBA', (width, height), background_color + (200,))
        draw = ImageDraw.Draw(image)
        
        # Add text
        try:
            font = ImageFont.truetype(self.font_path, 36)
        except:
            font = ImageFont.load_default()
        
        # Center text
        bbox = draw.textbbox((0, 0), text, font=font)
        text_width = bbox[2] - bbox[0]
        text_height = bbox[3] - bbox[1]
        x = (width - text_width) // 2
        y = (height - text_height) // 2
        
        draw.text((x, y), text, fill=text_color, font=font)
        
        # Convert to base64
        buffer = io.BytesIO()
        image.save(buffer, format='PNG')
        buffer.seek(0)
        
        return base64.b64encode(buffer.getvalue()).decode('utf-8')
    
    async def show_kill_streak(self, kills):
        banner_data = self.create_banner_image(
            f"KILL STREAK: {kills}!",
            background_color=(200, 0, 0)
        )
        
        await self.send_banner_command('obs_banner_set_data', {
            'content_data': banner_data,
            'content_type': 'image/png'
        })
        
        await self.send_banner_command('obs_banner_show')
        
        # Hide after 3 seconds
        await asyncio.sleep(3)
        await self.send_banner_command('obs_banner_hide')
    
    async def show_level_up(self, level):
        banner_data = self.create_banner_image(
            f"LEVEL UP! Level {level}",
            background_color=(0, 200, 0)
        )
        
        await self.send_banner_command('obs_banner_set_data', {
            'content_data': banner_data,
            'content_type': 'image/png'
        })
        
        await self.send_banner_command('obs_banner_show')
        
        await asyncio.sleep(4)
        await self.send_banner_command('obs_banner_hide')
    
    async def send_banner_command(self, action_id, parameters=None):
        message = {
            "path": "/api/v1/integration/sdk/action/invoke",
            "verb": "SET",
            "payload": {
                "actionId": action_id,
                "parameters": parameters or {}
            }
        }
        
        await self.ws.send(json.dumps(message))

# Usage example
async def game_event_handler(websocket, path):
    banner = GameEventBanner(websocket)
    
    # Simulate game events
    await asyncio.sleep(2)
    await banner.show_kill_streak(5)
    
    await asyncio.sleep(10)
    await banner.show_level_up(25)
```

### 4. Dynamic Content Updates

Regularly update banner with fresh content.

```javascript
class DynamicBannerUpdater {
  constructor(websocket) {
    this.ws = websocket;
    this.updateInterval = null;
  }
  
  startUpdating(intervalMs = 30000) { // 30 seconds
    this.updateInterval = setInterval(() => {
      this.updateBanner();
    }, intervalMs);
  }
  
  stopUpdating() {
    if (this.updateInterval) {
      clearInterval(this.updateInterval);
      this.updateInterval = null;
    }
  }
  
  async updateBanner() {
    try {
      // Fetch latest content from API
      const response = await fetch('https://api.example.com/latest-banner');
      const data = await response.json();
      
      if (data.banner_url) {
        // Use URL method
        await this.setBannerFromURL(data.banner_url);
      } else if (data.banner_data) {
        // Use direct data method
        await this.setBannerFromData(data.banner_data, data.content_type);
      }
      
      // Show banner if we have content
      if (data.show_banner) {
        await this.showBanner();
      } else {
        await this.hideBanner();
      }
      
    } catch (error) {
      console.error('Banner update failed:', error);
    }
  }
  
  async setBannerFromURL(url) {
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_set_url",
        parameters: {
          content_url: url
        }
      }
    };
    
    this.ws.send(JSON.stringify(message));
  }
  
  async setBannerFromData(base64Data, contentType) {
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_set_data",
        parameters: {
          content_data: base64Data,
          content_type: contentType
        }
      }
    };
    
    this.ws.send(JSON.stringify(message));
  }
  
  async showBanner() {
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_show",
        parameters: {}
      }
    };
    
    this.ws.send(JSON.stringify(message));
  }
  
  async hideBanner() {
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: {
        actionId: "obs_banner_hide",
        parameters: {}
      }
    };
    
    this.ws.send(JSON.stringify(message));
  }
}

// Usage
const updater = new DynamicBannerUpdater(websocket);
updater.startUpdating(60000); // Update every minute
```

## Testing

### Simple Test Script

```bash
#!/bin/bash
# Simple curl test for WebSocket (requires websocat or similar)

echo "Testing OBS Plugin WebSocket API"

# Install websocat if needed:
# cargo install websocat

# Test connection and send commands
websocat ws://localhost:9001 <<EOF
{"path":"/api/v1/integration/sdk/action/invoke","verb":"SET","payload":{"actionId":"obs_banner_show","parameters":{}}}
{"path":"/api/v1/integration/sdk/action/invoke","verb":"SET","payload":{"actionId":"obs_stream_start","parameters":{}}}
EOF
```

### Debug Helper

```javascript
// Debug helper for testing WebSocket commands
class OBSDebugger {
  constructor() {
    this.ws = null;
    this.connect();
  }
  
  connect() {
    this.ws = new WebSocket('ws://localhost:9001');
    
    this.ws.onopen = () => {
      console.log('✅ Connected to OBS Plugin');
      this.runTests();
    };
    
    this.ws.onmessage = (event) => {
      console.log('📨 Received:', event.data);
    };
    
    this.ws.onerror = (error) => {
      console.error('❌ WebSocket error:', error);
    };
    
    this.ws.onclose = () => {
      console.log('🔌 Disconnected from OBS Plugin');
    };
  }
  
  sendCommand(actionId, parameters = {}) {
    const message = {
      path: "/api/v1/integration/sdk/action/invoke",
      verb: "SET",
      payload: { actionId, parameters }
    };
    
    console.log('📤 Sending:', JSON.stringify(message, null, 2));
    this.ws.send(JSON.stringify(message));
  }
  
  async runTests() {
    console.log('🧪 Running basic tests...');
    
    // Test banner visibility
    await this.delay(1000);
    this.sendCommand('obs_banner_show');
    
    await this.delay(2000);
    this.sendCommand('obs_banner_hide');
    
    await this.delay(1000);
    this.sendCommand('obs_banner_toggle');
    
    console.log('✅ Tests completed');
  }
  
  delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
  }
}

// Run debugger
const debugger = new OBSDebugger();
```

These examples provide a solid foundation for integrating the VortiDeck OBS Plugin into your applications.