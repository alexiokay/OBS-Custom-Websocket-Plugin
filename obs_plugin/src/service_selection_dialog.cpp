#include "service_selection_dialog.hpp"
#include "mdns_discovery.hpp"
#include <obs.hpp>
#include <QFont>
#include <QScreen>
#include <QDebug>
#include <QApplication>

using namespace vorti::applets::obs_plugin;

ServiceSelectionDialog::ServiceSelectionDialog(const std::vector<ServiceInfo>& services, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowCloseButtonHint | Qt::WindowTitleHint)
    , ui(std::make_unique<Ui::ServiceSelectionDialog>())
    , m_services(services)
    , m_selectedIndex(-1)
    , m_accepted(false)
{
    ui->setupUi(this);
    
    // Ensure Connect button starts disabled until an item is properly selected
    ui->connectButton->setEnabled(false);
    
    // Set window behavior - don't stay on top when parent is minimized
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowFlags(windowFlags() & ~Qt::WindowStaysOnTopHint);
    
    populateServiceList();
    
    // Set dialog properties
    setModal(true);
    setFixedSize(600, 450);
    
    // Center the dialog on screen
    if (parent) {
        // Center relative to parent
        move(parent->geometry().center() - rect().center());
    } else {
        // Center on primary screen
        QScreen* primaryScreen = QApplication::primaryScreen();
        if (primaryScreen) {
            QRect screenGeometry = primaryScreen->geometry();
            move(screenGeometry.center() - rect().center());
        }
    }
    
    // Bring to front and activate
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    activateWindow();
    raise();
    
    // Connect signals
    connect(ui->serviceList, &QListWidget::itemSelectionChanged, this, &ServiceSelectionDialog::onItemSelectionChanged);
    connect(ui->connectButton, &QPushButton::clicked, this, &ServiceSelectionDialog::onConnectClicked);
    connect(ui->serviceList, &QListWidget::itemDoubleClicked, this, &ServiceSelectionDialog::onItemDoubleClicked);
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

void ServiceSelectionDialog::populateServiceList()
{
    for (size_t i = 0; i < m_services.size(); ++i) {
        const auto& service = m_services[i];
        
        // Create simple display text without emojis
        QString displayText = QString("%1 (%2:%3)")
                             .arg(QString::fromStdString(service.name))
                             .arg(QString::fromStdString(service.ip_address))
                             .arg(service.port);
        
        QListWidgetItem* item = new QListWidgetItem(displayText);
        item->setData(Qt::UserRole, static_cast<int>(i)); // Store index
        
        // Set simple tooltip
        item->setToolTip(QString("VortiDeck Service\nAddress: %1:%2\nURL: %3")
                        .arg(QString::fromStdString(service.ip_address))
                        .arg(service.port)
                        .arg(QString::fromStdString(service.websocket_url)));
        
        // Mark preferred service (port 9001) 
        if (service.port == 9001) {
            displayText = QString("[RECOMMENDED] %1 (%2:%3)")
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
    
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_services.size())) {
        qDebug() << "Valid selection, accepting dialog";
        m_accepted = true;
        accept();
    } else {
        qDebug() << "Invalid selection, not accepting dialog";
    }
}

void ServiceSelectionDialog::onItemDoubleClicked(QListWidgetItem* item)
{
    if (item) {
        m_selectedIndex = item->data(Qt::UserRole).toInt();
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_services.size())) {
            m_accepted = true;
            accept();
        }
    }
}