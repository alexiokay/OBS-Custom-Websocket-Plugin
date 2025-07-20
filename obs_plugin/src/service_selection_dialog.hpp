#pragma once

#include "ui_ServiceSelectionDialog.h"

#include <obs.hpp>
#include <QDialog>
#include <QListWidgetItem>
#include <vector>
#include <string>
#include <memory>
#include <map>

// Include the full ServiceInfo definition for Qt MOC
#include "mdns_discovery.hpp"

class ServiceSelectionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ServiceSelectionDialog(const std::vector<vorti::applets::obs_plugin::ServiceInfo>& services, QWidget *parent = nullptr);
    virtual ~ServiceSelectionDialog();
    
    // Get the selected service URL, empty if cancelled
    std::string getSelectedServiceUrl() const;
    
    // Get the index of the selected service, -1 if cancelled
    int getSelectedServiceIndex() const;
    
    // Get the security code entered by the user
    std::string getSecurityCode() const;
    
    // Update connection status for a specific service
    void updateServiceStatus(int serviceIndex, bool connected);
    
    // Update the service list with new discoveries
    void updateServiceList(const std::vector<vorti::applets::obs_plugin::ServiceInfo>& services);
    
    // Show/hide connecting progress indicator
    void showConnectingProgress(const std::string& message);
    void hideConnectingProgress();
    
    // Handle auto-connection in progress
    void showAutoConnectProgress(const std::string& serviceName, const std::string& url);
    
    // Show that a service is already connected (no security code needed)
    void markServiceAsConnected(int serviceIndex);
    
    // Trusted device storage
    bool isDeviceTrusted(const std::string& deviceId) const;
    void saveAsTrustedDevice(const std::string& deviceId, const std::string& securityCode);
    std::string getTrustedDeviceCode(const std::string& deviceId) const;

signals:
    void refreshRequested();

private slots:
    void onItemSelectionChanged();
    void onConnectClicked();
    void onItemDoubleClicked(QListWidgetItem* item);
    void onBackClicked();
    void onSecurityCodeChanged();
    void onRefreshClicked();

private:
    void populateServiceList();
    void showSecurityCodePage();
    void showServiceSelectionPage();
    void updateConnectButtonState();
    
    // UI generated from .ui file
    std::unique_ptr<Ui::ServiceSelectionDialog> ui;
    
    // Data
    std::vector<vorti::applets::obs_plugin::ServiceInfo> m_services;
    std::string m_selectedUrl;
    int m_selectedIndex;
    bool m_accepted;
    std::string m_securityCode;
    QTimer* m_authenticationTimer = nullptr;  // Track authentication timer to cancel if needed
    
    // Trusted device storage (deviceId -> securityCode)
    std::map<std::string, std::string> m_trustedDevices;
};