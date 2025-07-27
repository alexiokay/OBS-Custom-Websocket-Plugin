#include "service_selection_dialog.hpp"
#include "mdns_discovery.hpp"
#include <obs.hpp>
#include <QFont>
#include <QScreen>
#include <QDebug>
#include <QApplication>
#include <QTimer>

using namespace vorti::applets::obs_plugin;

ServiceSelectionDialog::ServiceSelectionDialog(const std::vector<ServiceInfo>& services, QWidget *parent)
    : QDialog(parent)
    , ui(std::make_unique<Ui::ServiceSelectionDialog>())
    , m_services(services)
    , m_selectedIndex(-1)
    , m_accepted(false)
{
    try {
        ui->setupUi(this);
        
        // Set safe dialog properties
        setModal(true);
        setAttribute(Qt::WA_DeleteOnClose, false);
        setWindowTitle("VortiDeck Service Selection");
        resize(600, 450);
        
        // Ensure Connect button starts disabled
        if (ui->connectButton) {
            ui->connectButton->setEnabled(false);
        }
        
        populateServiceList();
        
        // Connect signals with error checking
        if (ui->serviceList) {
            connect(ui->serviceList, &QListWidget::itemSelectionChanged, this, &ServiceSelectionDialog::onItemSelectionChanged);
            connect(ui->serviceList, &QListWidget::itemDoubleClicked, this, &ServiceSelectionDialog::onItemDoubleClicked);
        }
        if (ui->connectButton) {
            connect(ui->connectButton, &QPushButton::clicked, this, &ServiceSelectionDialog::onConnectClicked);
        }
        if (ui->backButton) {
            connect(ui->backButton, &QPushButton::clicked, this, &ServiceSelectionDialog::onBackClicked);
        }
        if (ui->securityCodeInput) {
            connect(ui->securityCodeInput, &QLineEdit::textChanged, this, &ServiceSelectionDialog::onSecurityCodeChanged);
        }
        if (ui->refreshButton) {
            connect(ui->refreshButton, &QPushButton::clicked, this, &ServiceSelectionDialog::onRefreshClicked);
        }
        
        qDebug() << "ServiceSelectionDialog created successfully";
    } catch (const std::exception& e) {
        qDebug() << "Exception in ServiceSelectionDialog constructor:" << e.what();
    }
}

ServiceSelectionDialog::~ServiceSelectionDialog()
{
    try {
        qDebug() << "ServiceSelectionDialog destructor called";
        
        // Cancel any pending authentication timer
        if (m_authenticationTimer) {
            m_authenticationTimer->stop();
            m_authenticationTimer->deleteLater();
            m_authenticationTimer = nullptr;
        }
        
        // Qt handles UI cleanup automatically with unique_ptr
    } catch (const std::exception& e) {
        qDebug() << "Exception in ServiceSelectionDialog destructor:" << e.what();
    }
}

std::string ServiceSelectionDialog::getSelectedServiceUrl() const
{
    if (m_accepted && m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_services.size())) {
        return m_services[m_selectedIndex].websocket_url;
    }
    return "";
}

int ServiceSelectionDialog::getSelectedServiceIndex() const
{
    return m_accepted ? m_selectedIndex : -1;
}

std::string ServiceSelectionDialog::getSecurityCode() const
{
    return m_securityCode;
}

void ServiceSelectionDialog::populateServiceList()
{
    // Handle empty services list
    if (m_services.empty()) {
        QListWidgetItem* item = new QListWidgetItem("🔍 No VortiDeck services found on network");
        item->setData(Qt::UserRole, -1); // Invalid index
        item->setToolTip("Make sure your VortiDeck device is powered on and connected to the same network");
        
        // Make item non-selectable
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        
        ui->serviceList->addItem(item);
        ui->connectButton->setEnabled(false);
        return;
    }

    for (size_t i = 0; i < m_services.size(); ++i) {
        const auto& service = m_services[i];
        
        // Add connection status indicator
        QString statusIcon = "🔴"; // Red circle for disconnected
        QString statusText = "Disconnected";
        
        // Create display text with status
        QString displayText = QString("%1 %2 (%3:%4)")
                             .arg(statusIcon)
                             .arg(QString::fromStdString(service.name))
                             .arg(QString::fromStdString(service.ip_address))
                             .arg(service.port);
        
        QListWidgetItem* item = new QListWidgetItem(displayText);
        item->setData(Qt::UserRole, static_cast<int>(i)); // Store index
        item->setData(Qt::UserRole + 1, statusText); // Store status text
        
        // Set detailed tooltip with connection status
        item->setToolTip(QString("VortiDeck Service\nAddress: %1:%2\nURL: %3\nStatus: %4")
                        .arg(QString::fromStdString(service.ip_address))
                        .arg(service.port)
                        .arg(QString::fromStdString(service.websocket_url))
                        .arg(statusText));
        
        // Mark preferred service (port 9001) 
        if (service.port == 9001) {
            displayText = QString("%1 [RECOMMENDED] %2 (%3:%4)")
                         .arg(statusIcon)
                         .arg(QString::fromStdString(service.name))
                         .arg(QString::fromStdString(service.ip_address))
                         .arg(service.port);
            item->setText(displayText);
        }
        
        ui->serviceList->addItem(item);
    }
    
    // Select the first item by default (preferably the recommended one)
    if (ui->serviceList->count() > 0) {
        int selectedIndex = 0;
        
        // Look for recommended service first
        for (int i = 0; i < ui->serviceList->count(); ++i) {
            QListWidgetItem* item = ui->serviceList->item(i);
            int serviceIndex = item->data(Qt::UserRole).toInt();
            if (serviceIndex < static_cast<int>(m_services.size()) && m_services[serviceIndex].port == 9001) {
                selectedIndex = i;
                break;
            }
        }
        
        // Properly select the item (not just highlight)
        QListWidgetItem* itemToSelect = ui->serviceList->item(selectedIndex);
        if (itemToSelect) {
            ui->serviceList->setCurrentItem(itemToSelect);
            itemToSelect->setSelected(true);
            
            // Manually update internal state and enable Connect button
            m_selectedIndex = itemToSelect->data(Qt::UserRole).toInt();
            ui->connectButton->setEnabled(true);
            
            // Trigger selection changed signal to ensure consistency
            onItemSelectionChanged();
            
            qDebug() << "Auto-selected service at index:" << selectedIndex << "with service index:" << m_selectedIndex;
        }
    }
}

void ServiceSelectionDialog::onItemSelectionChanged()
{
    QList<QListWidgetItem*> selectedItems = ui->serviceList->selectedItems();
    
    qDebug() << "Selection changed, selected items count:" << selectedItems.size();
    
    if (!selectedItems.isEmpty()) {
        QListWidgetItem* selectedItem = selectedItems.first();
        m_selectedIndex = selectedItem->data(Qt::UserRole).toInt();
        ui->connectButton->setEnabled(true);
        qDebug() << "Item selected, index:" << m_selectedIndex << "Connect button enabled";
    } else {
        m_selectedIndex = -1;
        ui->connectButton->setEnabled(false);
        qDebug() << "No item selected, Connect button disabled";
    }
}

void ServiceSelectionDialog::onConnectClicked()
{
    qDebug() << "Connect button clicked! Selected index:" << m_selectedIndex << "Services count:" << m_services.size();
    
    // If we're on the service selection page, move to security code page
    if (ui->stackedWidget->currentIndex() == 0) {
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_services.size())) {
            showSecurityCodePage();
        }
    }
    // If we're on the security code page, validate and accept
    else if (ui->stackedWidget->currentIndex() == 1) {
        QString code = ui->securityCodeInput->text().trimmed();
        if (code.length() == 6) {
            // Show connecting progress
            showConnectingProgress("🔐 Authenticating with security code...");
            
            // Cancel any existing authentication timer
            if (m_authenticationTimer) {
                m_authenticationTimer->stop();
                m_authenticationTimer->deleteLater();
                m_authenticationTimer = nullptr;
            }
            
            // Create new authentication timer
            m_authenticationTimer = new QTimer(this);
            m_authenticationTimer->setSingleShot(true);
            connect(m_authenticationTimer, &QTimer::timeout, this, [this, code]() {
                m_securityCode = code.toStdString();
                
                // Save as trusted device for future use
                if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_services.size())) {
                    const auto& service = m_services[m_selectedIndex];
                    std::string deviceId = service.ip_address + ":" + std::to_string(service.port);
                    saveAsTrustedDevice(deviceId, code.toStdString());
                }
                
                m_accepted = true;
                accept();
            });
            m_authenticationTimer->start(1500);
        }
    }
}

void ServiceSelectionDialog::onItemDoubleClicked(QListWidgetItem* item)
{
    if (item) {
        m_selectedIndex = item->data(Qt::UserRole).toInt();
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_services.size())) {
            showSecurityCodePage();
        }
    }
}

void ServiceSelectionDialog::onBackClicked()
{
    showServiceSelectionPage();
}

void ServiceSelectionDialog::onSecurityCodeChanged()
{
    updateConnectButtonState();
}

void ServiceSelectionDialog::showSecurityCodePage()
{
    ui->stackedWidget->setCurrentIndex(1);
    ui->backButton->setVisible(true);
    ui->connectButton->setText("Authenticate");
    
    // Check if this device is trusted and auto-fill security code
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_services.size())) {
        const auto& service = m_services[m_selectedIndex];
        std::string deviceId = service.ip_address + ":" + std::to_string(service.port);
        
        if (isDeviceTrusted(deviceId)) {
            std::string trustedCode = getTrustedDeviceCode(deviceId);
            ui->securityCodeInput->setText(QString::fromStdString(trustedCode));
            ui->securityInstructionLabel->setText("This device is trusted. The security code has been filled automatically.");
        } else {
            ui->securityCodeInput->clear();
            ui->securityInstructionLabel->setText("Enter the 6-digit security code displayed on your VortiDeck device:");
        }
    }
    
    ui->securityCodeInput->setFocus();
    updateConnectButtonState();
}

void ServiceSelectionDialog::showServiceSelectionPage()
{
    ui->stackedWidget->setCurrentIndex(0);
    ui->backButton->setVisible(false);
    ui->connectButton->setText("Connect");
    ui->securityCodeInput->clear();
    updateConnectButtonState();
}

void ServiceSelectionDialog::updateConnectButtonState()
{
    if (ui->stackedWidget->currentIndex() == 0) {
        // Service selection page - enable if service is selected
        ui->connectButton->setEnabled(m_selectedIndex >= 0);
    } else {
        // Security code page - enable if 6 digits entered
        QString code = ui->securityCodeInput->text().trimmed();
        ui->connectButton->setEnabled(code.length() == 6);
    }
}

void ServiceSelectionDialog::updateServiceStatus(int serviceIndex, bool connected)
{
    // Find the list item corresponding to this service
    for (int i = 0; i < ui->serviceList->count(); ++i) {
        QListWidgetItem* item = ui->serviceList->item(i);
        if (item && item->data(Qt::UserRole).toInt() == serviceIndex) {
            const auto& service = m_services[serviceIndex];
            
            // Update status icon and text
            QString statusIcon = connected ? "🟢" : "🔴"; // Green for connected, red for disconnected
            QString statusText = connected ? "Connected" : "Disconnected";
            
            // Recreate display text with new status
            QString displayText = QString("%1 %2 (%3:%4)")
                                 .arg(statusIcon)
                                 .arg(QString::fromStdString(service.name))
                                 .arg(QString::fromStdString(service.ip_address))
                                 .arg(service.port);
            
            // Mark preferred service (port 9001) 
            if (service.port == 9001) {
                displayText = QString("%1 [RECOMMENDED] %2 (%3:%4)")
                             .arg(statusIcon)
                             .arg(QString::fromStdString(service.name))
                             .arg(QString::fromStdString(service.ip_address))
                             .arg(service.port);
            }
            
            item->setText(displayText);
            item->setData(Qt::UserRole + 1, statusText);
            
            // Update tooltip
            item->setToolTip(QString("VortiDeck Service\nAddress: %1:%2\nURL: %3\nStatus: %4")
                            .arg(QString::fromStdString(service.ip_address))
                            .arg(service.port)
                            .arg(QString::fromStdString(service.websocket_url))
                            .arg(statusText));
            break;
        }
    }
}

void ServiceSelectionDialog::showConnectingProgress(const std::string& message)
{
    ui->progressLabel->setText(QString::fromStdString(message));
    ui->progressLabel->setVisible(true);
    ui->connectButton->setEnabled(false);
    ui->backButton->setEnabled(false);
    ui->cancelButton->setEnabled(false);
    ui->securityCodeInput->setEnabled(false);
}

void ServiceSelectionDialog::hideConnectingProgress()
{
    ui->progressLabel->setVisible(false);
    ui->connectButton->setEnabled(true);
    ui->backButton->setEnabled(true);
    ui->cancelButton->setEnabled(true);
    ui->securityCodeInput->setEnabled(true);
    updateConnectButtonState();
}

bool ServiceSelectionDialog::isDeviceTrusted(const std::string& deviceId) const
{
    return m_trustedDevices.find(deviceId) != m_trustedDevices.end();
}

void ServiceSelectionDialog::saveAsTrustedDevice(const std::string& deviceId, const std::string& securityCode)
{
    m_trustedDevices[deviceId] = securityCode;
    
    // TODO: Persist to file for permanent storage
    // For now, just store in memory for the session
    qDebug() << "Saved trusted device:" << QString::fromStdString(deviceId);
}

std::string ServiceSelectionDialog::getTrustedDeviceCode(const std::string& deviceId) const
{
    auto it = m_trustedDevices.find(deviceId);
    return (it != m_trustedDevices.end()) ? it->second : "";
}

void ServiceSelectionDialog::onRefreshClicked()
{
    // Show refreshing state
    ui->refreshButton->setText("Refreshing...");
    ui->refreshButton->setEnabled(false);
    
    // Emit signal to request refresh from main plugin
    emit refreshRequested();
    
    // Re-enable button after a short delay
    QTimer::singleShot(2000, this, [this]() {
        ui->refreshButton->setText("Refresh");
        ui->refreshButton->setEnabled(true);
    });
}

void ServiceSelectionDialog::updateServiceList(const std::vector<vorti::applets::obs_plugin::ServiceInfo>& services)
{
    // Update internal services list
    m_services = services;
    
    // Clear and repopulate the list
    ui->serviceList->clear();
    m_selectedIndex = -1;
    
    populateServiceList();
    
    qDebug() << "Service list updated with" << services.size() << "services";
}

void ServiceSelectionDialog::showAutoConnectProgress(const std::string& serviceName, const std::string& url)
{
    // Update the dialog to show that auto-connection is happening
    ui->titleLabel->setText("Auto-Connection in Progress");
    ui->instructionLabel->setText(QString("Automatically connecting to: %1").arg(QString::fromStdString(serviceName)));
    
    // Disable the connect button and show progress
    ui->connectButton->setEnabled(false);
    ui->connectButton->setText("Connecting...");
    
    // Show progress in the service list
    ui->serviceList->clear();
    QListWidgetItem* item = new QListWidgetItem(QString("🔄 Connecting to %1...").arg(QString::fromStdString(serviceName)));
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    ui->serviceList->addItem(item);
    
    qDebug() << "Showing auto-connect progress for" << QString::fromStdString(serviceName);
}

void ServiceSelectionDialog::markServiceAsConnected(int serviceIndex)
{
    if (serviceIndex >= 0 && serviceIndex < static_cast<int>(m_services.size())) {
        // Update the service status to connected
        updateServiceStatus(serviceIndex, true);
        
        // If this is the selected service, auto-fill the trusted code and show connection status
        if (serviceIndex == m_selectedIndex) {
            const auto& service = m_services[serviceIndex];
            std::string deviceId = service.ip_address + ":" + std::to_string(service.port);
            
            // Mark as trusted device with empty code (already connected)
            saveAsTrustedDevice(deviceId, "");
            
            // Update instruction to show it's already connected
            if (ui->stackedWidget->currentIndex() == 1) { // Security code page
                ui->securityInstructionLabel->setText("✅ This device is already connected! No security code required.");
                ui->securityCodeInput->setText("------");
                ui->securityCodeInput->setEnabled(false);
                ui->connectButton->setText("Already Connected");
                ui->connectButton->setEnabled(false);
            }
        }
        
        qDebug() << "Marked service" << serviceIndex << "as already connected";
    }
}