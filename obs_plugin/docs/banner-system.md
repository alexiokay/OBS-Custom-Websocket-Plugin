# VortiDeck Banner System Documentation

## Overview

The VortiDeck Banner System provides dynamic advertisement integration for OBS Studio, with different capabilities for free and premium users. The system ensures consistent monetization while offering flexibility for premium subscribers.

## Banner Queue & Rotation System

### 🔄 **How Banner Rotation Works**

The banner system uses an intelligent queue-based approach for handling multiple advertisements:

**Architecture:**
- **Receive multiple banners** → Store in `m_banner_queue`
- **Keep ONE banner source** → Update content dynamically  
- **Rotate on timer** → Change banner content every 30 seconds
- **Seamless transitions** → Users see different ads automatically

### 📈 **Benefits**

- **Professional ad rotation** - Like real advertising systems
- **Memory efficient** - One source, multiple contents
- **Free users get variety** - Different ads keep engagement
- **Premium users control** - Can set rotation speed via API

### 🛠️ **Technical Implementation**

```cpp
// Banner Queue Structure
std::vector<BannerContent> m_banner_queue;
std::atomic<size_t> m_current_banner_index{0};
std::atomic<int> m_rotation_interval_seconds{30};

// Rotation Logic
void rotate_banner_content() {
    if (m_banner_queue.empty()) return;
    
    // Get next banner in queue
    size_t next_index = (m_current_banner_index + 1) % m_banner_queue.size();
    BannerContent& next_banner = m_banner_queue[next_index];
    
    // Update source content dynamically
    update_banner_source_content(next_banner.data, next_banner.type);
    m_current_banner_index = next_index;
}
```

### ⚙️ **Configuration Options**

| Setting | Free Users | Premium Users |
|---------|------------|---------------|
| Rotation Speed | Fixed 30 seconds | Customizable via API |
| Banner Control | Automatic only | Manual + Automatic |
| Queue Size | Up to 5 banners | Unlimited |
| Content Types | Standard ads | All content types |

## User Types & Permissions

### 🆓 **Free Users**
- **Monetization Model**: 5% revenue share with VortiDeck
- **Banner Frequency**: Every 5 minutes
- **Banner Control**: Limited (banners are enforced)
- **Auto-Restoration**: Banners restore after 10 seconds if hidden
- **Positioning**: Locked to bottom of screen
- **Protection**: Enhanced system prevents removal/modification

### 💎 **Premium Users**
- **Monetization Model**: Keep 100% revenue
- **Banner Control**: Complete freedom via VortiDeck platform
- **Custom Content**: Curated partnerships & custom ads via VortiDeck platform
- **Custom Positioning**: Full control over banner placement
- **Persistence Mode**: Optional banner enforcement
- **Platform Integration**: Pre-configured animations, positions, partnerships

## Banner Source Integration

### 🎯 **OBS Source Registration**

The VortiDeck Banner is now available as a proper OBS source:

1. **Add Through Sources Interface**:
   - Open OBS Studio
   - Click "+" in Sources panel
   - Select "VortiDeck Banner"
   - Configure content and settings

2. **Properties Panel**:
   - Content Type: Color, Text, Image, Video
   - Content Data: Hex colors, text, file paths, URLs
   - Dimensions: Width and Height controls
   - Rotation Settings: Timer intervals (Premium only)

### 🔧 **Source Features**

| Feature | Description |
|---------|-------------|
| **Dynamic Content** | Updates content without recreating source |
| **Metadata Marking** | Automatic VortiDeck banner identification |
| **Clean Naming** | "VortiDeck Banner" (no underscores) |
| **Duplicate Prevention** | One banner per scene maximum |
| **Professional Integration** | Seamless OBS workflow |

## Content Types Support

### 🎨 **Content Distribution Model**

| User Type | Content Source | Description | Examples |
|-----------|---------------|-------------|----------|
| **🆓 Free Users** | VortiDeck Ad Network | Standard advertisement pool | Network sponsors, general ads |
| **💎 Premium Users** | Custom Partnerships | Curated partnerships via VortiDeck platform | Brand collaborations, sponsored content |
| **💎 Premium Users** | Custom Campaigns | Personalized ad campaigns | Custom branding, targeted messaging |

### 🛠️ **Technical Content Support**

| Content Type | Description | Example |
|--------------|-------------|---------|
| **Solid Color** | RGB hex colors | `#FF0000` (Red) |
| **Text** | Dynamic text banners | "Subscribe Now!" |
| **Image** | Static images | PNG, JPG, GIF |
| **Video** | Video advertisements | MP4, AVI, MOV |
| **URL** | Web-based content | HTTP/HTTPS links |

### 🔄 **Content Rotation**

```json
{
  "banner_queue": [
    {
      "type": "color",
      "data": "#FF0000",
      "duration": 30
    },
    {
      "type": "text", 
      "data": "VortiDeck - Stream Better",
      "duration": 30
    },
    {
      "type": "image",
      "data": "/path/to/banner.png",
      "duration": 30
    }
  ]
}
```

## Signal-Based Protection

### 🛡️ **Protection Mechanisms**

For **Free Users**, the system provides robust banner protection:

1. **Removal Protection**: Automatically recreates deleted banners
2. **Visibility Protection**: Restores hidden banners after 10 seconds
3. **Position Protection**: Locks banner to designated area
4. **Signal Monitoring**: Real-time event detection and response

### 📊 **Monitoring Events**

| Event | Response | User Type |
|-------|----------|-----------|
| `item_remove` | Immediate recreation | Free Users |
| `item_visible` | Delayed restoration (10s) | Free Users |
| `item_transform` | Position correction | Free Users |
| `scene_change` | Banner initialization | Free Users |

## API Integration

### 🔒 **API Access Control**

**⚠️ Important**: The VortiDeck Banner API is **exclusively for the VortiDeck application**. End users do not have direct API access.

- **VortiDeck App**: Full API control for banner management
- **End Users**: Interaction only via VortiDeck platform interface
- **OBS Users**: No direct API access - all configuration via VortiDeck platform

### 🌐 **Connection-Based Banner Behavior**

The banner system behaves differently based on VortiDeck app connection status:

#### **🆓 Free Users**
- **Without Connection**: Default banners appear immediately (ensures monetization)
- **With Connection**: Real-time network ads from VortiDeck ad pool
- **Connection Lost**: Maintains last received banners, shows defaults if needed

#### **💎 Premium Users**
- **Without Connection**: Pre-configured banners from VortiDeck platform
- **With Connection**: Fresh partnership content and custom campaigns
- **Connection Lost**: Continues with cached/pre-configured content

**Configuration Source**: All banner settings (animations, positions, partnerships) are pre-configured on the VortiDeck platform and automatically synchronized to the plugin.

### 🔌 **WebSocket Commands** (VortiDeck App Only)

```json
{
  "action": "banner_set_queue",
  "parameters": {
    "banners": [
      {
        "type": "partnership",
        "data": "brand_campaign_001",
        "duration": 45
      }
    ],
    "rotation_speed": 30
  }
}
```

### 📡 **Available Actions** (VortiDeck App Only)

| Action | Description | Free Users | Premium Users |
|--------|-------------|------------|---------------|
| `banner_show` | Show current banner | ✅ Auto-triggered | ✅ Platform-controlled |
| `banner_hide` | Hide banner | ❌ Not allowed | ✅ Platform-controlled |
| `banner_toggle` | Toggle visibility | ❌ Not allowed | ✅ Platform-controlled |
| `banner_set_data` | Update content | ✅ Network ads | ✅ Custom content |
| `banner_set_queue` | Set rotation queue | ✅ Limited (5 max) | ✅ Unlimited |
| `banner_set_rotation` | Set rotation speed | ❌ Fixed 30s | ✅ Customizable |
| `banner_set_partnerships` | Set partnership content | ❌ Not available | ✅ Platform-managed |

## File Structure

### 📁 **Banner Assets**

- **Directory**: `%TEMP%/vortideck_banners/` (Windows) or `/tmp/vortideck_banners/` (macOS)
- **Cleanup**: Automatic cleanup on plugin unload
- **Formats**: PNG, JPG, GIF, MP4, AVI, MOV supported
- **Size Limits**: 10MB per banner asset

### 🗂️ **Configuration Files**

```
obs_plugin/
├── src/
│   ├── banner_manager.cpp      # Core banner logic
│   ├── banner_manager.hpp      # Banner interface
│   └── obs_plugin.cpp          # OBS integration
├── docs/
│   ├── banner-system.md        # This documentation
│   └── BANNER_API.md           # API reference
└── CMakeLists.txt              # Build configuration
```

## Troubleshooting

### 🔍 **Common Issues**

| Problem | Solution |
|---------|----------|
| Banner not showing | Check if source is added to scene |
| Multiple banners | Use metadata-based detection |
| Banner disappears | Check user type and permissions |
| Content not updating | Verify API payload format |
| Rotation stopped | Check timer jthread status |

### 🧪 **Debug Commands**

```cpp
// Enable debug logging
#define BANNER_DEBUG_LOGGING 1

// Check banner status
log_message("Banner queue size: " + std::to_string(m_banner_queue.size()));
log_message("Current banner index: " + std::to_string(m_current_banner_index));
log_message("Rotation interval: " + std::to_string(m_rotation_interval_seconds));
```

## Version History

### v1.2.0 - Banner Queue System
- ✅ Added banner queue and rotation
- ✅ Implemented OBS source registration
- ✅ Fixed banner naming (removed underscores)
- ✅ Enhanced duplicate prevention
- ✅ Professional ad rotation system

### v1.1.0 - Metadata-Based Detection
- ✅ Replaced name-based with metadata detection
- ✅ Fixed banner restoration issues
- ✅ Improved duplicate prevention
- ✅ Enhanced signal-based protection

### v1.0.0 - Initial Release
- ✅ Basic banner functionality
- ✅ Free vs Premium user system
- ✅ WebSocket API integration
- ✅ Signal-based enforcement