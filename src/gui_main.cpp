#include "common.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

using namespace gs;

static QJsonObject callHelper(const QJsonObject &req, QString *err = nullptr) {
    QLocalSocket sock;
    sock.connectToServer(socketPath());
    if (!sock.waitForConnected(5000)) {
        if (err) *err = "Could not connect to helper at " + socketPath();
        return {};
    }

    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    sock.write(line);
    if (!sock.waitForBytesWritten(5000)) {
        if (err) *err = "Failed to send request";
        return {};
    }
    if (!sock.waitForReadyRead(30000)) {
        if (err) *err = "No response from helper";
        return {};
    }

    const QByteArray resp = sock.readLine().trimmed();
    const auto doc = QJsonDocument::fromJson(resp);
    if (!doc.isObject()) {
        if (err) *err = "Invalid helper response";
        return {};
    }

    const QJsonObject obj = doc.object();
    if (!obj.value("ok").toBool()) {
        if (err) *err = obj.value("error").toString("Helper error");
    }
    return obj;
}

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        setWindowTitle("GPU Switcher");
        resize(1180, 900);

        auto *central = new QWidget(this);
        auto *root = new QVBoxLayout(central);

        auto *title = new QLabel("<h2>GPU Switcher</h2><p>VFIO helper for controlled Linux host ↔ Windows VM GPU ownership.</p>");
        summary = new QLabel("Loading status...");
        summary->setWordWrap(true);
        root->addWidget(title);
        root->addWidget(summary);

        auto *form = new QFormLayout();
        vmName = new QLineEdit();
        vmUuid = new QLineEdit();
        gpuBdf = new QLineEdit();
        audioBdf = new QLineEdit();
        preferredBoot = new QComboBox();
        preferredBoot->addItems({"host", "vm"});
        vendorReset = new QCheckBox("Try vendor-reset on supported AMD hardware");
        fallbackDisplay = new QCheckBox("I have fallback display / SSH access");
        allowSingleGpu = new QCheckBox("I understand single-GPU mode is fragile");
        autoStartVm = new QCheckBox("Auto-start VM after reboot into VM mode");
        thermalGuard = new QCheckBox("Enable GPU temperature guard");
        maxGpuTemp = new QSpinBox();
        maxGpuTemp->setRange(60, 105);
        maxGpuTemp->setSuffix(" °C");
        maxGpuTemp->setToolTip("Blocks VM mode when Linux can read the GPU temperature and it is at or above this value.");
        safetyRecoveryMinutes = new QSpinBox();
        safetyRecoveryMinutes->setRange(0, 240);
        safetyRecoveryMinutes->setSuffix(" min");
        safetyRecoveryMinutes->setToolTip("After the VM stops, automatically reboot to Host after this many minutes unless you choose another action. 0 disables the timer.");

        form->addRow("VM name", vmName);
        form->addRow("VM UUID", vmUuid);
        form->addRow("GPU PCI BDF", gpuBdf);
        form->addRow("Audio PCI BDF", audioBdf);
        form->addRow("Preferred boot", preferredBoot);
        form->addRow("", vendorReset);
        form->addRow("", fallbackDisplay);
        form->addRow("", allowSingleGpu);
        form->addRow("", autoStartVm);
        form->addRow("", thermalGuard);
        form->addRow("Max GPU temperature", maxGpuTemp);
        form->addRow("VM-stop safety recovery", safetyRecoveryMinutes);

        auto *setupBox = new QGroupBox("Setup");
        setupBox->setLayout(form);
        root->addWidget(setupBox);

        auto *buttons = new QHBoxLayout();
        probeBtn = new QPushButton("Probe");
        saveBtn = new QPushButton("Save");
        autoSetupBtn = new QPushButton("Auto setup single GPU");
        hookBtn = new QPushButton("Install hook");
        inventoryBtn = new QPushButton("Inventory");
        diagnoseBtn = new QPushButton("Preflight");
        simulateBtn = new QPushButton("Simulate VM");
        vmBtn = new QPushButton("Reboot to VM");
        hostBtn = new QPushButton("Reboot to Host");
        refreshBtn = new QPushButton("Refresh status");
        buttons->addWidget(probeBtn);
        buttons->addWidget(saveBtn);
        buttons->addWidget(autoSetupBtn);
        buttons->addWidget(hookBtn);
        buttons->addWidget(inventoryBtn);
        buttons->addWidget(diagnoseBtn);
        buttons->addWidget(simulateBtn);
        buttons->addWidget(vmBtn);
        buttons->addWidget(hostBtn);
        buttons->addWidget(refreshBtn);
        root->addLayout(buttons);

        auto *decisionLayout = new QHBoxLayout();
        vmStopStatus = new QLabel("No stopped-VM decision is pending.");
        vmStopStatus->setWordWrap(true);
        restartHostNowBtn = new QPushButton("Restart now to Host");
        hostNextRestartBtn = new QPushButton("Return on next restart");
        keepVmBtn = new QPushButton("Keep GPU with VM");
        decisionLayout->addWidget(vmStopStatus, 1);
        decisionLayout->addWidget(restartHostNowBtn);
        decisionLayout->addWidget(hostNextRestartBtn);
        decisionLayout->addWidget(keepVmBtn);

        auto *decisionBox = new QGroupBox("After the Windows VM closes");
        decisionBox->setLayout(decisionLayout);
        root->addWidget(decisionBox);

        log = new QPlainTextEdit();
        log->setReadOnly(true);
        log->setPlaceholderText("Logs and compatibility checks appear here.");
        root->addWidget(log, 1);

        setCentralWidget(central);
        statusBar()->showMessage("Ready");

        connect(probeBtn, &QPushButton::clicked, this, &MainWindow::onProbe);
        connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSave);
        connect(autoSetupBtn, &QPushButton::clicked, this, &MainWindow::onAutoSetupSingleGpu);
        connect(hookBtn, &QPushButton::clicked, this, &MainWindow::onInstallHook);
        connect(inventoryBtn, &QPushButton::clicked, this, &MainWindow::onInventory);
        connect(diagnoseBtn, &QPushButton::clicked, this, &MainWindow::onDiagnose);
        connect(simulateBtn, &QPushButton::clicked, this, &MainWindow::onSimulateVm);
        connect(vmBtn, &QPushButton::clicked, this, &MainWindow::onSwitchToVm);
        connect(hostBtn, &QPushButton::clicked, this, &MainWindow::onSwitchToHost);
        connect(refreshBtn, &QPushButton::clicked, this, [this]() { refreshStatus(true); });
        connect(restartHostNowBtn, &QPushButton::clicked, this, &MainWindow::onRestartHostNow);
        connect(hostNextRestartBtn, &QPushButton::clicked, this, &MainWindow::onReturnHostNextRestart);
        connect(keepVmBtn, &QPushButton::clicked, this, &MainWindow::onKeepGpuForVm);

        autoRefresh = new QTimer(this);
        connect(autoRefresh, &QTimer::timeout, this, [this]() { refreshStatus(false); });
        autoRefresh->start(10000);

        setVmStopDecisionEnabled(false);
        refreshFromConfig();
        refreshStatus(true);
    }

private slots:
    void onProbe() {
        QString err;
        auto resp = callHelper({{"cmd", "probe"}, {"gpuBdf", gpuBdf->text().trimmed()}}, &err);
        if (!err.isEmpty()) {
            appendLog("Probe failed: " + err);
            return;
        }
        appendLog("Probe response:\n" + QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
    }

    void onSave() {
        QString err;
        QJsonObject req{
            {"cmd", "saveConfig"},
            {"vmName", vmName->text().trimmed()},
            {"vmUuid", vmUuid->text().trimmed()},
            {"gpuBdf", gpuBdf->text().trimmed()},
            {"audioBdf", audioBdf->text().trimmed()},
            {"preferredBoot", preferredBoot->currentText()},
            {"useVendorReset", vendorReset->isChecked()},
            {"hasFallbackDisplay", fallbackDisplay->isChecked()},
            {"allowSingleGpu", allowSingleGpu->isChecked()},
            {"autoStartVmOnBoot", autoStartVm->isChecked()},
            {"thermalGuardEnabled", thermalGuard->isChecked()},
            {"maxGpuTempC", maxGpuTemp->value()},
            {"safetyAutoRecoveryMinutes", safetyRecoveryMinutes->value()}
        };
        auto resp = callHelper(req, &err);
        if (!err.isEmpty()) {
            appendLog("Save failed: " + err);
            return;
        }
        appendLog("Config saved.");
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        refreshStatus(false);
    }

    void onAutoSetupSingleGpu() {
        const auto answer = QMessageBox::warning(
            this,
            "Auto setup single GPU",
            "This will save the current VM/GPU fields, enable single-GPU acknowledgement, install the libvirt hook, and add missing GPU/audio PCI hostdev entries to the VM XML after making a backup. Continue?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) return;

        QString err;
        QJsonObject req{
            {"cmd", "autoSetupSingleGpu"},
            {"vmName", vmName->text().trimmed()},
            {"vmUuid", vmUuid->text().trimmed()},
            {"gpuBdf", gpuBdf->text().trimmed()},
            {"audioBdf", audioBdf->text().trimmed()}
        };
        auto resp = callHelper(req, &err);
        if (!err.isEmpty()) {
            appendLog("Auto setup failed: " + err);
            return;
        }
        appendLog("Single-GPU auto setup completed.");
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        refreshFromConfig();
        refreshStatus(true);
    }

    void onInstallHook() {
        QString err;
        auto resp = callHelper({{"cmd", "installHook"}}, &err);
        if (!err.isEmpty()) {
            appendLog("Hook install failed: " + err);
            return;
        }
        appendLog("Hook installed at " + hookPath());
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        refreshStatus(false);
    }

    void onInventory() {
        QString err;
        auto resp = callHelper({{"cmd", "inventory"}}, &err);
        if (!err.isEmpty()) {
            appendLog("Inventory failed: " + err);
            return;
        }
        appendLog(renderInventoryText(resp.value("inventory").toObject().toVariantMap()));
    }

    void onDiagnose() {
        QString err;
        QJsonObject req{{"cmd", "diagnose"}};
        if (!gpuBdf->text().trimmed().isEmpty()) req["gpuBdf"] = gpuBdf->text().trimmed();
        auto resp = callHelper(req, &err);
        if (!err.isEmpty()) {
            appendLog("Diagnostics failed: " + err);
            return;
        }
        appendLog(renderPreflightText(resp.value("diagnostic").toObject().toVariantMap()));
    }

    void onSimulateVm() {
        QString err;
        QJsonObject req{{"cmd", "simulateVm"}};
        if (!gpuBdf->text().trimmed().isEmpty()) req["gpuBdf"] = gpuBdf->text().trimmed();
        auto resp = callHelper(req, &err);
        if (!err.isEmpty()) {
            appendLog("Simulation failed: " + err);
            return;
        }
        const auto sim = resp.value("simulation").toObject();
        appendLog("Simulation flow: " + sim.value("flow").toString());
        appendLog("Simulation state: " + sim.value("state").toString());
        appendLog("Planned phases: " + QString::number(sim.value("phases").toArray().size()));
        appendLog(renderPreflightText(sim.value("preflight").toObject().toVariantMap()));
    }

    void onSwitchToVm() {
        if (!confirmReboot("Reboot to VM mode", "The PC will restart and the GPU will be bound to vfio-pci for the VM. Continue?")) return;
        QString err;
        auto resp = callHelper({{"cmd", "switchToVm"}}, &err);
        if (!err.isEmpty()) {
            appendLog("Switch-to-VM blocked or failed: " + err);
            return;
        }
        appendLog("Reboot to VM scheduled successfully.");
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        statusBar()->showMessage("System will reboot into VM mode");
    }

    void onSwitchToHost() {
        if (!confirmReboot("Reboot to Host mode", "The PC will restart and try to return the GPU to the Linux host driver. Continue?")) return;
        QString err;
        auto resp = callHelper({{"cmd", "switchToHost"}}, &err);
        if (!err.isEmpty()) {
            appendLog("Switch-to-host failed: " + err);
            return;
        }
        appendLog("Reboot to host scheduled successfully.");
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        statusBar()->showMessage("System will reboot back into host mode");
    }

    void onRestartHostNow() {
        if (!confirmReboot("Restart now to Host", "The PC will restart now and restore the GPU to the Linux host. Continue?")) return;
        QString err;
        auto resp = callHelper({{"cmd", "restartHostNow"}}, &err);
        if (!err.isEmpty()) {
            appendLog("Could not schedule immediate host recovery: " + err);
            return;
        }
        appendLog("Immediate host recovery scheduled.");
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        statusBar()->showMessage("System will reboot to Host mode");
    }

    void onReturnHostNextRestart() {
        QString err;
        auto resp = callHelper({{"cmd", "returnHostNextRestart"}}, &err);
        if (!err.isEmpty()) {
            appendLog("Could not schedule next-restart host recovery: " + err);
            return;
        }
        appendLog("Host recovery will happen on the next restart. No reboot was started now.");
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        refreshStatus(false);
    }

    void onKeepGpuForVm() {
        const auto answer = QMessageBox::question(
            this,
            "Keep GPU with VM",
            "This clears the stopped-VM warning, cancels the safety recovery timer, and leaves the GPU assigned to VM/VFIO mode until you manually choose Reboot to Host. Continue only if guest/firmware cooling is confirmed.",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) return;

        QString err;
        auto resp = callHelper({{"cmd", "keepGpuForVm"}}, &err);
        if (!err.isEmpty()) {
            appendLog("Could not keep GPU assigned to VM: " + err);
            return;
        }
        appendLog("GPU will stay assigned to the VM until manually changed.");
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        refreshStatus(false);
    }

private:
    void refreshStatus(bool verbose = false) {
        QString err;
        auto resp = callHelper({{"cmd", "status"}}, &err);
        if (!err.isEmpty()) {
            if (err != lastStatusError || verbose) appendLog("Status unavailable: " + err);
            lastStatusError = err;
            summary->setText("Status unavailable: " + err);
            setVmStopDecisionEnabled(false);
            return;
        }
        lastStatusError.clear();

        const auto inventory = resp.value("inventory").toObject().toVariantMap();
        const auto preflight = resp.value("preflight").toObject().toVariantMap();
        const auto config = resp.value("config").toObject().toVariantMap();
        summary->setText(renderSummary(inventory, preflight));
        updateVmStopDecisionUi(config, inventory);

        if (verbose) {
            appendLog("Status:\n" + QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
            appendLog(renderInventoryText(inventory));
            appendLog(renderPreflightText(preflight));
        }
    }

    QString renderSummary(const QVariantMap &inventory, const QVariantMap &preflight) const {
        const bool ready = preflight.value("canEnterVm").toBool();
        const auto blockers = preflight.value("blockers").toList();
        const auto warnings = preflight.value("warnings").toList();
        QString text = ready ? "System is ready for VM mode." : "VM mode is blocked.";
        text += " GPUs: " + QString::number(inventory.value("graphicsDevices").toList().size());
        text += " | Mode: " + inventory.value("currentMode").toString();
        text += " | Secure Boot: " + QString(inventory.value("secureBootEnabled").toBool() ? "enabled" : "disabled/unknown");
        const auto comp = preflight.value("compatibility").toMap();
        text += " | IOMMU: " + QString(comp.value("iommuEnabled").toBool() ? "enabled" : "missing");
        text += " | GPU temp: " + QString(comp.value("gpuTemperatureReadable").toBool() ? QString::number(comp.value("gpuTemperatureC").toInt()) + " °C" : "unreadable");
        const auto nextBoot = inventory.value("nextBootMode").toString();
        if (!nextBoot.isEmpty()) text += " | Next boot: " + nextBoot;
        if (inventory.value("vmStoppedAwaitingDecision").toBool()) text += " | VM stopped: decision needed";
        if (!blockers.isEmpty()) text += " | Blockers: " + QString::number(blockers.size());
        if (!warnings.isEmpty()) text += " | Warnings: " + QString::number(warnings.size());
        return text;
    }

    void updateVmStopDecisionUi(const QVariantMap &config, const QVariantMap &inventory) {
        const bool awaiting = config.value("vmStoppedAwaitingDecision", inventory.value("vmStoppedAwaitingDecision")).toBool();
        const QString stoppedVm = config.value("lastStoppedVm", inventory.value("lastStoppedVm")).toString();
        const QString stoppedAt = config.value("lastStoppedAt", inventory.value("lastStoppedAt")).toString();
        const QString nextBoot = inventory.value("nextBootMode").toString();
        const QString safetyDeadline = config.value("safetyRecoveryDeadline").toString();
        const int safetyMinutes = config.value("safetyAutoRecoveryMinutes").toInt();
        const bool hostRecoveryScheduled = nextBoot == "host" && inventory.value("currentMode").toString() == "vm";
        if (awaiting) setVmStopDecisionButtons(true, true, true);
        else if (hostRecoveryScheduled) setVmStopDecisionButtons(true, false, true);
        else setVmStopDecisionButtons(false, false, false);

        if (awaiting) {
            QString text = "The configured VM has stopped, and the GPU is still assigned to VM/VFIO mode.";
            if (!stoppedVm.isEmpty()) text += " VM: " + stoppedVm + ".";
            if (!stoppedAt.isEmpty()) text += " Stopped at UTC: " + stoppedAt + ".";
            text += " Host recovery is already scheduled for the next restart.";
            if (safetyMinutes > 0 && !safetyDeadline.isEmpty()) text += " Safety auto-reboot deadline UTC: " + safetyDeadline + ".";
            text += " Choose whether to reboot now, keep the next-restart recovery, or intentionally keep the GPU with the VM.";
            vmStopStatus->setText(text);
            statusBar()->showMessage("VM stopped; choose GPU recovery action");
        } else {
            if (nextBoot == "host") {
                QString text = "Host recovery is scheduled for the next restart. The GPU will return to Linux when the machine reboots.";
                if (safetyMinutes > 0 && !safetyDeadline.isEmpty()) text += " Safety auto-reboot deadline UTC: " + safetyDeadline + ".";
                vmStopStatus->setText(text);
            } else {
                vmStopStatus->setText("No stopped-VM decision is pending.");
            }
        }
    }

    void setVmStopDecisionEnabled(bool enabled) {
        setVmStopDecisionButtons(enabled, enabled, enabled);
    }

    void setVmStopDecisionButtons(bool restartNow, bool nextRestart, bool keepVm) {
        restartHostNowBtn->setEnabled(restartNow);
        hostNextRestartBtn->setEnabled(nextRestart);
        keepVmBtn->setEnabled(keepVm);
    }

    bool confirmReboot(const QString &title, const QString &message) {
        const auto answer = QMessageBox::warning(
            this,
            title,
            message,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        return answer == QMessageBox::Yes;
    }

    void refreshFromConfig() {
        const AppConfig cfg = loadConfig();
        vmName->setText(cfg.vmName);
        vmUuid->setText(cfg.vmUuid);
        gpuBdf->setText(cfg.gpuBdf);
        audioBdf->setText(cfg.audioBdf);
        vendorReset->setChecked(cfg.useVendorReset);
        fallbackDisplay->setChecked(cfg.hasFallbackDisplay);
        allowSingleGpu->setChecked(cfg.allowSingleGpu);
        autoStartVm->setChecked(cfg.autoStartVmOnBoot);
        thermalGuard->setChecked(cfg.thermalGuardEnabled);
        maxGpuTemp->setValue(cfg.maxGpuTempC);
        safetyRecoveryMinutes->setValue(cfg.safetyAutoRecoveryMinutes);
        const int idx = preferredBoot->findText(cfg.preferredBoot);
        if (idx >= 0) preferredBoot->setCurrentIndex(idx);
    }

    void appendLog(const QString &msg) {
        log->appendPlainText(msg);
    }

    QLabel *summary{};
    QLabel *vmStopStatus{};
    QLineEdit *vmName{};
    QLineEdit *vmUuid{};
    QLineEdit *gpuBdf{};
    QLineEdit *audioBdf{};
    QComboBox *preferredBoot{};
    QCheckBox *vendorReset{};
    QCheckBox *fallbackDisplay{};
    QCheckBox *allowSingleGpu{};
    QCheckBox *autoStartVm{};
    QCheckBox *thermalGuard{};
    QSpinBox *maxGpuTemp{};
    QSpinBox *safetyRecoveryMinutes{};
    QPushButton *probeBtn{};
    QPushButton *saveBtn{};
    QPushButton *autoSetupBtn{};
    QPushButton *hookBtn{};
    QPushButton *inventoryBtn{};
    QPushButton *diagnoseBtn{};
    QPushButton *simulateBtn{};
    QPushButton *vmBtn{};
    QPushButton *hostBtn{};
    QPushButton *refreshBtn{};
    QPushButton *restartHostNowBtn{};
    QPushButton *hostNextRestartBtn{};
    QPushButton *keepVmBtn{};
    QPlainTextEdit *log{};
    QTimer *autoRefresh{};
    QString lastStatusError;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    return app.exec();
}

#include "gui_main.moc"
