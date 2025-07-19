# Enhanced VortiDeck Connection System

## Overview

This document outlines the implementation plan for an enhanced connection system that includes auto-connect functionality, security code authentication, connection status indicators, and trusted device management.

## Features to Implement

### 1. Auto-Connect for Single Service

When only one VortiDeck service is discovered on the network, the plugin should automatically connect without showing the service selection dialog.

```cpp
// In show_service_selection_dialog()
if (services_copy.size() == 1) {
    // Skip dialog and connect directly
    m_selected_service_url = services_copy[0].websocket_url;
    log_to_obs("Only one VortiDeck service found - auto-connecting...");
    // Trigger connection without showing dialog
    return;
}
```

### 2. Security Code Authentication System

Implement a two-step authentication flow to ensure secure connections between OBS and VortiDeck services.

#### Multi-Step Dialog Flow

```
Step 1: Service Selection
├─ List of discovered services
├─ Connection status indicator (✅ Connected / ❌ Disconnected)
└─ Connect button

Step 2: Security Code Entry
├─ Instructions: "Enter the security code from VortiDeck app"
├─ Code input field (6-8 digits)
├─ Back button (return to service list)
└─ Verify button (authenticate and connect)
```

#### Implementation Details

1. **Add QStackedWidget** to ServiceSelectionDialog for multiple pages
2. **Page 1**: Current service list view
3. **Page 2**: Security code entry with:
   - QLineEdit for code input (numeric only)
   - QLabel with clear instructions
   - Progress indicator during verification
   - Error handling for incorrect codes

### 3. Connection Status Indicator

Display the current connection status prominently at the top of the service selection dialog.

#### Visual States

- 🟢 **Connected** - "Connected to [Service Name]"
- 🔴 **Disconnected** - "Not Connected"
- 🟡 **Connecting** - "Connecting..."
- 🔒 **Secured** - "Secured Connection to [Service Name]"

#### Implementation

```cpp
// Add to ServiceSelectionDialog class
private:
    QLabel* m_connectionStatusLabel;
    
public:
    void updateConnectionStatus(ConnectionState state, const QString& serviceName = "");
    
enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    SecuredConnection,
    Error
};
```

### 4. Connection Progress Indicators

Provide visual feedback during the connection process.

#### In-Dialog Progress (Recommended)

```cpp
// Add to dialog
QProgressBar* m_connectionProgress;
QLabel* m_connectionStatusText;

// Show during connection
void showConnectingProgress() {
    m_connectionProgress->setRange(0, 0); // Indeterminate
    m_connectionStatusText->setText("Connecting to VortiDeck service...");
    m_connectButton->setEnabled(false);
    m_cancelButton->setVisible(true);
}

// Hide after connection
void hideConnectingProgress() {
    m_connectionProgress->setVisible(false);
    m_connectionStatusText->clear();
    m_connectButton->setEnabled(true);
    m_cancelButton->setVisible(false);
}
```

#### Alternative: OBS Status Bar Integration

```cpp
// Update OBS status bar for global visibility
void updateOBSStatusBar(const QString& message) {
    // Use OBS frontend API to show status
    obs_frontend_push_ui_translation(obs_module_get_string);
    // Implementation depends on OBS version
}
```

### 5. Trusted Device Management

Store and manage trusted connections to avoid repeated security code entry.

#### Data Structure

```cpp
struct TrustedDevice {
    std::string service_id;      // Unique service identifier (MAC or UUID)
    std::string device_hash;     // Secure hash of device credentials
    std::string friendly_name;   // User-friendly name (e.g., "Living Room PC")
    std::string ip_address;      // Last known IP address
    time_t trusted_date;         // When device was trusted
    time_t last_connected;       // Last successful connection
};

class TrustedDeviceManager {
public:
    void addTrustedDevice(const TrustedDevice& device);
    void removeTrustedDevice(const std::string& service_id);
    bool isDeviceTrusted(const std::string& service_id);
    std::vector<TrustedDevice> getTrustedDevices();
    void saveTrustedDevices();  // Persist to OBS config
    void loadTrustedDevices();  // Load from OBS config
};
```

## Enhanced UI Design

### ServiceSelectionDialog.ui Structure

```xml
<widget class="QDialog" name="ServiceSelectionDialog">
  <!-- Connection Status Banner -->
  <widget class="QFrame" name="connectionStatusFrame">
    <property name="styleSheet">
      <string>QFrame { background-color: #2d2d2d; border-bottom: 2px solid #464646; }</string>
    </property>
    <layout class="QHBoxLayout">
      <widget class="QLabel" name="connectionStatusIcon"/>
      <widget class="QLabel" name="connectionStatusLabel"/>
    </layout>
  </widget>
  
  <!-- Stacked Widget for Multiple Views -->
  <widget class="QStackedWidget" name="stackedWidget">
    <!-- Page 0: Service List -->
    <widget class="QWidget" name="serviceListPage">
      <layout class="QVBoxLayout">
        <widget class="QLabel" name="titleLabel">
          <property name="text">
            <string>Select VortiDeck Service</string>
          </property>
        </widget>
        <widget class="QListWidget" name="serviceList"/>
        <widget class="QProgressBar" name="connectionProgress">
          <property name="visible">
            <bool>false</bool>
          </property>
        </widget>
        <layout class="QHBoxLayout" name="buttonLayout">
          <widget class="QPushButton" name="cancelButton"/>
          <widget class="QPushButton" name="connectButton"/>
        </layout>
      </layout>
    </widget>
    
    <!-- Page 1: Security Code Entry -->
    <widget class="QWidget" name="securityCodePage">
      <layout class="QVBoxLayout">
        <widget class="QLabel" name="securityTitleLabel">
          <property name="text">
            <string>Security Verification Required</string>
          </property>
        </widget>
        <widget class="QLabel" name="securityInstructions">
          <property name="text">
            <string>A security code has been displayed in your VortiDeck app.
Please enter the code below to establish a secure connection.</string>
          </property>
          <property name="wordWrap">
            <bool>true</bool>
          </property>
        </widget>
        <widget class="QLineEdit" name="securityCodeInput">
          <property name="maxLength">
            <number>8</number>
          </property>
          <property name="placeholderText">
            <string>Enter 6-8 digit code</string>
          </property>
          <property name="inputMask">
            <string>99999999</string>
          </property>
        </widget>
        <widget class="QCheckBox" name="trustDeviceCheckbox">
          <property name="text">
            <string>Trust this device (won't ask for code again)</string>
          </property>
          <property name="checked">
            <bool>true</bool>
          </property>
        </widget>
        <widget class="QLabel" name="errorLabel">
          <property name="styleSheet">
            <string>QLabel { color: #ff4444; }</string>
          </property>
          <property name="visible">
            <bool>false</bool>
          </property>
        </widget>
        <layout class="QHBoxLayout">
          <widget class="QPushButton" name="backButton">
            <property name="text">
              <string>Back</string>
            </property>
          </widget>
          <widget class="QPushButton" name="verifyButton">
            <property name="text">
              <string>Verify &amp; Connect</string>
            </property>
            <property name="default">
              <bool>true</bool>
            </property>
          </widget>
        </layout>
      </layout>
    </widget>
  </widget>
</widget>
```

## WebSocket Protocol Extensions

### Authentication Message

```json
{
  "type": "AUTH_REQUEST",
  "payload": {
    "security_code": "123456",
    "device_info": {
      "hostname": "USER-PC",
      "platform": "Windows",
      "obs_version": "30.1.2",
      "plugin_version": "1.0.0"
    },
    "trust_device": true
  }
}
```

### Authentication Response

```json
{
  "type": "AUTH_RESPONSE",
  "payload": {
    "success": true,
    "device_token": "unique-device-token-here",
    "message": "Authentication successful"
  }
}
```

## Implementation Priority

1. **Phase 1: Core Features**
   - Auto-connect for single service
   - Connection status indicator
   - Basic security code dialog

2. **Phase 2: Enhanced Security**
   - Security code verification
   - Trusted device storage
   - Persistent authentication tokens

3. **Phase 3: User Experience**
   - Progress indicators
   - Error handling and recovery
   - Connection history

## Security Considerations

1. **Code Generation**: Security codes should be:
   - Generated on the VortiDeck app side
   - Time-limited (expire after 60 seconds)
   - Single-use only
   - Minimum 6 digits for security

2. **Device Trust**: When trusting a device:
   - Store secure hash, not plain credentials
   - Implement device revocation on VortiDeck app
   - Show trusted devices list in app settings
   - Auto-expire trust after 30 days of inactivity

3. **Network Security**:
   - Only allow connections from same subnet by default
   - Option to enable remote connections with additional verification
   - Use WSS (WebSocket Secure) when possible

## Error Handling

### Connection Failures
- Show clear error messages
- Provide retry options
- Fall back to manual service selection

### Authentication Failures
- "Incorrect security code" - allow retry
- "Code expired" - request new code
- "Too many attempts" - temporary lockout

### Network Issues
- "Service not responding"
- "Connection timeout"
- "Network unreachable"

## Testing Scenarios

1. **Single Service Discovery**
   - Verify auto-connect behavior
   - Test trusted vs untrusted devices

2. **Multiple Services**
   - Ensure dialog shows correctly
   - Test service switching

3. **Security Code Flow**
   - Valid code acceptance
   - Invalid code rejection
   - Code expiration handling

4. **Trust Management**
   - Adding trusted devices
   - Removing trusted devices
   - Trust expiration

5. **Edge Cases**
   - No services found
   - Service disappears during connection
   - Network interruptions
   - OBS shutdown during connection

## Future Enhancements

1. **QR Code Authentication**
   - Display QR code in dialog
   - Scan with VortiDeck mobile app

2. **Biometric Authentication**
   - Windows Hello integration
   - Touch ID/Face ID on macOS

3. **Remote Connection Support**
   - Cloud relay for remote streaming
   - Enhanced security for internet connections

4. **Multi-Profile Support**
   - Different security settings per scene collection
   - Quick switching between trusted devices