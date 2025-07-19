#pragma once

#include "ui_ServiceSelectionDialog.h"

#include <obs.hpp>
#include <QDialog>
#include <QListWidgetItem>
#include <vector>
#include <string>
#include <memory>

// Include the full ServiceInfo definition for Qt MOC
#include "mdns_discovery.hpp"

class ServiceSelectionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ServiceSelectionDialog(const std::vector<vorti::applets::obs_plugin::ServiceInfo>& services, QWidget *parent = nullptr);
    
    // Get the selected service URL, empty if cancelled
    std::string getSelectedServiceUrl() const;
    
    // Get the index of the selected service, -1 if cancelled
    int getSelectedServiceIndex() const;

private slots:
    void onItemSelectionChanged();
    void onConnectClicked();
    void onItemDoubleClicked(QListWidgetItem* item);

private:
    void populateServiceList();
    
    // UI generated from .ui file
    std::unique_ptr<Ui::ServiceSelectionDialog> ui;
    
    // Data
    std::vector<vorti::applets::obs_plugin::ServiceInfo> m_services;
    std::string m_selectedUrl;
    int m_selectedIndex;
    bool m_accepted;
};