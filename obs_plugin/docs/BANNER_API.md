# VortiDeck Banner API Documentation

## Overview

The **VortiDeck Creator Monetization System** provides a comprehensive banner management solution for OBS Studio with revenue sharing capabilities for creators.

## 🎯 Key Features

- **Creator Revenue Sharing**: 5% for free users, 80% for premium users
- **Automatic VortiDeck Ads**: Scheduled based on premium status
- **Professional Banner Management**: Multiple content types and positioning
- **Premium Status Verification**: Real-time status updates via WebSocket
- **User-Friendly Experience**: All banners are hideable and respectful
- **📊 Advanced Analytics**: Real-time ad tracking and performance metrics

---

## 📈 Analytics & Tracking System

### **Real-Time Ad Display Tracking**

The plugin automatically tracks and reports ad performance metrics back to VortiDeck:

**Tracked Metrics:**
- **Display Count**: How many times each ad was shown
- **Display Duration**: Actual time each ad was visible
- **View Count**: Estimated viewers during ad display
- **Completion Rate**: Percentage of ads shown to completion
- **Revenue Attribution**: Tracking by premium status and revenue share

**Automatic Reporting:**
```json
{
  "path": "/api/v1/integration/obs/analytics/ad-display",
  "verb": "SET",
  "payload": {
    "ad_id": "ad_12345",
    "campaign_id": "campaign_abc",
    "display_count": 1,
    "display_duration_ms": 30000,
    "planned_duration_ms": 30000,
    "completion_percentage": 100,
    "viewer_count": 1247,
    "premium_status": "false",
    "revenue_share": 0.05,
    "timestamp": "2024-01-15T10:30:00Z",
    "stream_metrics": {
      "bitrate": 6000,
      "framerate": 60,
      "resolution": "1920x1080"
    }
  }
}
```

### **Batch Analytics Reports**

Every 60 seconds, the plugin sends aggregated analytics:

```json
{
  "path": "/api/v1/integration/obs/analytics/batch-report",
  "verb": "SET", 
  "payload": {
    "report_period_start": "2024-01-15T10:00:00Z",
    "report_period_end": "2024-01-15T10:01:00Z",
    "total_ads_displayed": 12,
    "total_display_time_ms": 360000,
    "average_viewer_count": 1247,
    "premium_user": false,
    "revenue_generated": 2.45,
    "ads_by_campaign": [
      {
        "campaign_id": "campaign_abc",
        "displays": 8,
        "total_duration_ms": 240000,
        "completion_rate": 95.2
      },
      {
        "campaign_id": "campaign_def", 
        "displays": 4,
        "total_duration_ms": 120000,
        "completion_rate": 100.0
      }
    ]
  }
}
```

### **Stream Viewer Count Tracking**

**🔍 Available Methods:**

1. **OBS Studio Integration** (Most Accurate)
   - Access to streaming output statistics
   - Real-time bitrate and frame data
   - Can estimate viewer engagement

2. **Platform API Integration** (Requires Setup)
   - Twitch API: Real viewer counts
   - YouTube API: Live stream metrics
   - Facebook API: Live viewer data

3. **VortiDeck Network Data** (Recommended)
   - Cross-platform viewer aggregation
   - Real-time analytics dashboard
   - Multi-stream correlation

**Implementation Example:**
```json
{
  "path": "/api/v1/integration/obs/analytics/viewer-metrics",
  "verb": "SET",
  "payload": {
    "timestamp": "2024-01-15T10:30:00Z",
    "viewer_sources": {
      "twitch": 850,
      "youtube": 320,
      "facebook": 77,
      "total_estimated": 1247
    },
    "stream_quality": {
      "bitrate": 6000,
      "framerate": 60,
      "dropped_frames": 0.1
    },
    "ad_effectiveness": {
      "peak_viewers_during_ad": 1247,
      "viewer_retention_rate": 94.2
    }
  }
}
```

---

## 🎯 Campaign Performance Metrics

### **Revenue Attribution Tracking**

```json
{
  "path": "/api/v1/integration/obs/analytics/revenue-attribution",
  "verb": "SET",
  "payload": {
    "time_period": "2024-01-15T10:00:00Z/2024-01-15T11:00:00Z",
    "creator_revenue_share": 0.05,
    "total_ad_displays": 120,
    "estimated_revenue": 24.67,
    "creator_earnings": 1.23,
    "vortideck_earnings": 23.44,
    "performance_breakdown": {
      "high_performing_ads": 45,
      "medium_performing_ads": 52,
      "low_performing_ads": 23
    }
  }
}
```

### **A/B Testing Support**

```json
{
  "path": "/api/v1/integration/obs/analytics/ab-test-results",
  "verb": "SET",
  "payload": {
    "test_id": "banner_position_test_001",
    "variant_a": {
      "position": "bottom-left",
      "displays": 60,
      "completion_rate": 92.3,
      "viewer_retention": 94.1
    },
    "variant_b": {
      "position": "top-right", 
      "displays": 60,
      "completion_rate": 87.8,
      "viewer_retention": 96.2
    },
    "winning_variant": "variant_b",
    "statistical_significance": 0.95
  }
}
```

---

## 🔄 Real-Time Analytics WebSocket Actions

### **Enable Analytics Tracking**

**Action ID**: `obs_analytics_enable`

```json
{
  "actionId": "obs_analytics_enable",
  "parameters": {
    "tracking_level": "detailed",
    "report_interval_seconds": 60,
    "include_viewer_metrics": true,
    "include_revenue_attribution": true
  }
}
```

### **Request Analytics Report**

**Action ID**: `obs_analytics_get_report`

```json
{
  "actionId": "obs_analytics_get_report",
  "parameters": {
    "report_type": "campaign_performance",
    "time_range": "last_24_hours",
    "include_revenue": true
  }
}
```

**Response:**
```json
{
  "path": "/api/v1/integration/obs/analytics/report-response",
  "verb": "SET",
  "payload": {
    "report_type": "campaign_performance",
    "time_range": "last_24_hours",
    "total_displays": 2847,
    "total_revenue": 156.78,
    "creator_earnings": 7.84,
    "top_performing_campaigns": [
      {
        "campaign_id": "gaming_peripherals_001",
        "displays": 892,
        "completion_rate": 96.2,
        "revenue": 67.23
      }
    ]
  }
}
```

---

## 📊 Analytics Dashboard Integration

### **VortiDeck Analytics Dashboard**

The plugin feeds data into VortiDeck's analytics dashboard:

**Creator Dashboard Features:**
- Real-time earnings tracking
- Ad performance metrics
- Viewer engagement analytics
- Revenue optimization suggestions
- A/B test results

**VortiDeck Network Dashboard:**
- Cross-creator campaign performance
- Platform-wide analytics
- Revenue attribution models
- Inventory management
- Advertiser reporting

### **Third-Party Analytics Integration**

**Google Analytics Events:**
```json
{
  "actionId": "obs_analytics_google_event",
  "parameters": {
    "event_category": "banner_display",
    "event_action": "ad_shown",
    "event_label": "campaign_12345",
    "custom_metrics": {
      "revenue": 0.45,
      "viewer_count": 1247
    }
  }
}
```

**Facebook Pixel Integration:**
```json
{
  "actionId": "obs_analytics_facebook_pixel",
  "parameters": {
    "event_name": "VortiDeck_Ad_Display",
    "event_parameters": {
      "ad_id": "ad_12345",
      "value": 0.45,
      "currency": "USD"
    }
  }
}
```

---

## 💡 Stream Platform Integration

### **Multi-Platform Viewer Tracking**

**Twitch Integration:**
```javascript
// VortiDeck App Integration
const twitchAPI = new TwitchAPI(clientId, accessToken);
const viewerCount = await twitchAPI.getStreamViewers(channelId);

websocket.send(JSON.stringify({
  actionId: "obs_analytics_viewer_update",
  parameters: {
    platform: "twitch",
    viewer_count: viewerCount,
    timestamp: new Date().toISOString()
  }
}));
```

**YouTube Integration:**
```javascript
// VortiDeck App Integration
const youtubeAPI = new YouTubeAPI(apiKey);
const liveStats = await youtubeAPI.getLiveStreamStats(videoId);

websocket.send(JSON.stringify({
  actionId: "obs_analytics_viewer_update",
  parameters: {
    platform: "youtube",
    viewer_count: liveStats.concurrentViewers,
    timestamp: new Date().toISOString()
  }
}));
```

### **Cross-Platform Analytics Aggregation**

```json
{
  "path": "/api/v1/integration/obs/analytics/cross-platform-metrics",
  "verb": "SET",
  "payload": {
    "timestamp": "2024-01-15T10:30:00Z",
    "platforms": {
      "twitch": {
        "viewers": 850,
        "chatters": 342,
        "followers": 12750
      },
      "youtube": {
        "viewers": 320,
        "likes": 89,
        "subscribers": 8920
      },
      "facebook": {
        "viewers": 77,
        "reactions": 23,
        "shares": 5
      }
    },
    "total_reach": 1247,
    "ad_effectiveness_score": 87.3
  }
}
```

---

## 🎯 Implementation Priority

### **Phase 1: Basic Analytics (Immediate)**
- Ad display counting
- Duration tracking
- Basic reporting back to VortiDeck
- Revenue attribution

### **Phase 2: Advanced Metrics (Next)**
- Viewer count integration
- Platform API connections
- A/B testing framework
- Real-time dashboards

### **Phase 3: AI-Powered Optimization (Future)**
- Predictive analytics
- Automated campaign optimization
- Personalized ad targeting
- Dynamic pricing models

---

## 🔒 Privacy & Compliance

### **Data Collection Standards**
- **GDPR Compliant**: User consent and data rights
- **CCPA Compliant**: California privacy requirements
- **Anonymized Metrics**: No personal data collection
- **Opt-Out Options**: Users can disable tracking

### **Security Measures**
- **Encrypted Data Transfer**: All analytics encrypted
- **Secure Storage**: No local PII storage
- **Audit Trails**: Complete data access logging
- **Regular Security Reviews**: Quarterly assessments

---

## 🚀 Getting Started with Analytics

1. **Enable Analytics** in your VortiDeck dashboard
2. **Connect Platform APIs** (Twitch, YouTube, etc.)
3. **Configure Reporting** intervals and metrics
4. **Monitor Performance** via VortiDeck analytics
5. **Optimize Campaigns** based on data insights

**Ready to maximize your ad revenue with data-driven insights?** 📈💰

## 🕐 **Ad Duration & Window Management**

### **Maximum Ad Display Windows**

| User Type | Max Duration | Auto-Off Behavior | Queue Management |
|-----------|-------------|-------------------|------------------|
| **🆓 Free Users** | 60 seconds | Auto-rotates to next ad | Server-managed queue |
| **💎 Premium Users** | 300 seconds (5 min) | User-controlled | Client + Server hybrid |
| **🎯 VortiDeck Ads** | 45 seconds | Always auto-rotates | Network-managed |

### **Duration Control Parameters**

```json
{
  "actionId": "obs_banner_set_data",
  "content_data": "banner-url",
  "content_type": "image",
  "duration_seconds": 60,
  "auto_rotate": true,
  "premium_status": "false"
}
```

### **Window Lifecycle Management**

```json
{
  "actionId": "obs_banner_set_window",
  "window_id": "banner_001",
  "duration_seconds": 45,
  "auto_close": true,
  "next_banner_id": "banner_002"
}
```

**Auto-off Behavior:**
- Banner displays for its **individual duration** (not maximum window)
- Automatically switches to next ad when **individual duration** expires
- Maximum windows are just **safety limits** (caps for too-long ads)
- If no next banner, displays transparent placeholder
- Free users: Always enforced rotation
- Premium users: Optional auto-rotation

**Duration Priority & Window Management:**
1. **Individual ad duration** (e.g., 30 seconds)
2. **Maximum window limit** (e.g., 60 seconds for free users)
3. **Window tracking**: Cumulative time in current window
4. **Fair display rule**: Only show ad if it can display FULLY in remaining window time
5. **Result**: Ad shows for full duration OR gets moved to next window

**Window Management Logic:**
```
Window Time Used: 55 seconds (from previous ad)
Remaining Window: 5 seconds (60 - 55 = 5)
Next Ad Duration: 30 seconds

Decision: 30 > 5 → Cannot fit in current window
Action: Close current window, start fresh window for 30-second ad
Result: Ad gets full 30 seconds in new window
```

### **Queue Integration**

```json
{
  "actionId": "obs_banner_set_queue",
  "banners": [
    {
      "id": "banner_001",
      "content_data": "ad1.png",
      "content_type": "image",
      "duration_seconds": 30
    },
    {
      "id": "banner_002", 
      "content_data": "ad2.png",
      "content_type": "image",
      "duration_seconds": 45
    }
  ],
  "rotation_mode": "auto",
  "premium_status": "false"
}
``` 

## 🚀 **VortiDeck App Integration Guide**

### **1. Recommended Queue Management Strategy**

**🎯 Send Small Batches (3-5 ads) - Not Whole Queues**

```javascript
// ✅ RECOMMENDED: Send manageable batches
const sendAdBatch = () => {
    const nextAds = masterQueue.getNext(3); // Get next 3 ads
    
    websocket.send(JSON.stringify({
        actionId: "obs_banner_set_queue",
        banners: nextAds,
        premium_status: user.isPremium ? "true" : "false",
        batch_id: generateBatchId()
    }));
};

// ❌ AVOID: Sending massive queues
const sendEntireQueue = () => {
    websocket.send(JSON.stringify({
        actionId: "obs_banner_set_queue", 
        banners: masterQueue.getAll(), // 100+ ads - TOO MUCH!
        premium_status: user.isPremium ? "true" : "false"
    }));
};
```

### **2. Optimal Interface Workflow**

```javascript
class VortiDeckAdManager {
    constructor() {
        this.masterQueue = new AdQueue();
        this.pluginQueue = { size: 0, needsRefill: false };
        this.currentUser = null;
    }

    // Step 1: Initialize plugin with first batch
    async initializePlugin(user) {
        this.currentUser = user;
        const initialBatch = this.generateOptimalBatch(user, 3);
        
        await this.sendToPlugin({
            actionId: "obs_banner_set_queue",
            banners: initialBatch,
            premium_status: user.isPremium ? "true" : "false",
            auto_rotation: !user.isPremium, // Force rotation for free users
            batch_id: this.generateBatchId()
        });
    }

    // Step 2: Monitor plugin queue status
    handlePluginMessage(message) {
        if (message.type === "queue_status") {
            this.pluginQueue = message.queue_info;
            
            // Refill when queue gets low
            if (this.pluginQueue.size <= 1) {
                this.refillPluginQueue();
            }
        }
    }

    // Step 3: Send refill batches
    async refillPluginQueue() {
        const refillBatch = this.generateOptimalBatch(this.currentUser, 2);
        
        await this.sendToPlugin({
            actionId: "obs_banner_add_to_queue", // Add to existing queue
            banners: refillBatch,
            premium_status: this.currentUser.isPremium ? "true" : "false"
        });
    }

    // Step 4: Handle individual ad requests
    async showSingleAd(adContent) {
        await this.sendToPlugin({
            actionId: "obs_banner_set_data",
            content_data: adContent.url,
            content_type: adContent.type,
            duration_seconds: adContent.duration,
            premium_status: this.currentUser.isPremium ? "true" : "false"
        });
    }
}
```

### **3. WebSocket Message Formats**

**A. Initialize Plugin Queue (Start of Session)**
```json
{
    "actionId": "obs_banner_set_queue",
    "banners": [
        {
            "id": "ad_001",
            "content_data": "https://cdn.vortideck.com/ad1.png",
            "content_type": "image",
            "duration_seconds": 30,
            "campaign_id": "camp_123",
            "priority": "high"
        },
        {
            "id": "ad_002", 
            "content_data": "https://cdn.vortideck.com/ad2.mp4",
            "content_type": "video",
            "duration_seconds": 45,
            "campaign_id": "camp_124",
            "priority": "medium"
        }
    ],
    "premium_status": "false",
    "auto_rotation": true,
    "batch_id": "batch_001"
}
```

**B. Refill Plugin Queue (During Session)**
```json
{
    "actionId": "obs_banner_add_to_queue",
    "banners": [
        {
            "id": "ad_003",
            "content_data": "https://cdn.vortideck.com/ad3.png", 
            "content_type": "image",
            "duration_seconds": 25,
            "campaign_id": "camp_125"
        }
    ],
    "premium_status": "false",
    "append_to_existing": true
}
```

**C. Show Single Ad (Immediate Display)**
```json
{
    "actionId": "obs_banner_set_data",
    "content_data": "https://cdn.vortideck.com/urgent-ad.png",
    "content_type": "image", 
    "duration_seconds": 20,
    "premium_status": "false",
    "interrupt_current": true
}
```

### **4. Queue Management Scenarios**

**Scenario 1: Free User - Custom Content**
```javascript
// Free user gets custom banner content
const generateFreeUserBatch = () => {
    return [
        {
            id: "free_ad_001",
            content_data: "https://user-content.vortideck.com/banner1.png",
            content_type: "image",
            duration_seconds: 30,
            campaign_id: "user_campaign"
        },
        {
            id: "free_ad_002",
            content_data: "#FF6B35",
            content_type: "color",
            duration_seconds: 45,
            campaign_id: "user_campaign"
        }
    ];
};
```

**Scenario 2: Premium User - Custom Content**
```javascript
// Premium user gets custom/partnership content
const generatePremiumUserBatch = () => {
    return [
        {
            id: "custom_ad_001",
            content_data: "https://user-content.vortideck.com/custom1.png",
            content_type: "image",
            duration_seconds: 60,
            campaign_id: "user_campaign_123",
            css: "/* Custom animations */",
            css_locked: false
        },
        {
            id: "partnership_ad_001", 
            content_data: "https://partners.vortideck.com/brand-collab.mp4",
            content_type: "video",
            duration_seconds: 90,
            campaign_id: "partnership_456"
        }
    ];
};
```

### **5. Error Handling & Edge Cases**

```javascript
class RobustAdManager {
    async sendToPlugin(message) {
        try {
            await this.websocket.send(JSON.stringify(message));
            this.logAdAction("sent", message);
        } catch (error) {
            this.handleSendError(error, message);
        }
    }

    handleSendError(error, message) {
        // Store failed messages for retry
        this.failedMessages.push(message);
        
        // Fallback to single ad if queue fails
        if (message.actionId === "obs_banner_set_queue") {
            this.fallbackToSingleAd(message.banners[0]);
        }
    }

    async fallbackToSingleAd(ad) {
        await this.sendToPlugin({
            actionId: "obs_banner_set_data",
            content_data: ad.content_data,
            content_type: ad.content_type,
            duration_seconds: ad.duration_seconds,
            premium_status: this.currentUser.isPremium ? "true" : "false"
        });
    }
}
``` 

### **6. Recommended Session Workflow**

```javascript
class VortiDeckPlugin {
    async startSession(user) {
        // Step 1: Initialize with first batch (3 ads)
        await this.initializePlugin(user);
        
        // Step 2: Start monitoring plugin status
        this.startStatusMonitoring();
        
        // Step 3: Handle real-time events
        this.setupEventHandlers();
    }

    async initializePlugin(user) {
        const initialBatch = this.generateAdBatch(user, 3);
        
        await this.sendMessage({
            actionId: "obs_banner_set_queue",
            banners: initialBatch,
            premium_status: user.isPremium ? "true" : "false",
            auto_rotation: !user.isPremium // Force rotation for free users
        });
    }

    startStatusMonitoring() {
        // Monitor every 30 seconds
        setInterval(() => {
            this.checkPluginQueueStatus();
        }, 30000);
    }

    async checkPluginQueueStatus() {
        // Plugin should send queue status messages
        // When queue size <= 1, refill it
        if (this.pluginQueue.size <= 1) {
            await this.refillQueue();
        }
    }

    async refillQueue() {
        const refillBatch = this.generateAdBatch(this.currentUser, 2);
        
        await this.sendMessage({
            actionId: "obs_banner_add_to_queue", // Add to existing queue
            banners: refillBatch,
            premium_status: this.currentUser.isPremium ? "true" : "false"
        });
    }

    // For urgent/priority ads
    async showImmediateAd(adContent) {
        await this.sendMessage({
            actionId: "obs_banner_set_data",
            content_data: adContent.url,
            content_type: adContent.type,
            duration_seconds: adContent.duration,
            premium_status: this.currentUser.isPremium ? "true" : "false"
        });
    }
}
```

### **7. When to Use Each Method**

| Method | Use Case | Queue Size | Timing |
|--------|----------|------------|--------|
| **obs_banner_set_queue** | Start session, Replace entire queue | 3-5 ads | Session start |
| **obs_banner_add_to_queue** | Refill existing queue | 1-3 ads | When queue low |
| **obs_banner_set_data** | Single urgent ad | 1 ad | Immediate display |


### **8. Queue Refill Triggers**

```javascript
class QueueManager {
    handlePluginMessage(message) {
        switch (message.type) {
            case "queue_status":
                // Plugin reports: { size: 1, current_ad: "ad_002" }
                if (message.queue_info.size <= 1) {
                    this.refillQueue();
                }
                break;
                
            case "ad_completed":
                // Plugin reports: { ad_id: "ad_001", duration_shown: 30 }
                this.trackAdCompletion(message.ad_info);
                break;
                
            case "window_closed":
                // Plugin reports: { window_id: "window_001", ads_shown: 2 }
                this.handleWindowClosed(message.window_info);
                break;
        }
    }

    async refillQueue() {
        const newAds = this.generateAdBatch(this.currentUser, 2);
        
        await this.sendMessage({
            actionId: "obs_banner_add_to_queue",
            banners: newAds,
            premium_status: this.currentUser.isPremium ? "true" : "false"
        });
    }
}
``` 