
#include "common.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

using namespace gs;

static QJsonObject callHelper(const QJsonObject &req, QString *err = nullptr) {
    QLocalSocket sock;
    sock.connectToServer(socketPath());
    if (!sock.waitForConnected(3000)) {
        if (err) *err = "Could not connect to helper at " + socketPath();
        return {};
    }
    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    sock.write(line);
    if (!sock.waitForBytesWritten(3000)) {
        if (err) *err = "Failed to send request";
        return {};
    }
    if (!sock.waitForReadyRead(3000)) {
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
        resize(1120, 860);

        auto *central = new QWidget(this);
        auto *root = new QVBoxLayout(central);

        auto *title = new QLabel("<h2>GPU Switcher</h2><p>VFIO helper for safe GPU host↔VM transitions.</p>");
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

        form->addRow("VM name", vmName);
        form->addRow("VM UUID", vmUuid);
        form->addRow("GPU PCI BDF", gpuBdf);
        form->addRow("Audio PCI BDF", audioBdf);
        form->addRow("Preferred boot", preferredBoot);
        form->addRow("", vendorReset);
        form->addRow("", fallbackDisplay);
        form->addRow("", allowSingleGpu);
        form->addRow("", autoStartVm);

        auto *setupBox = new QGroupBox("Setup");
        setupBox->setLayout(form);
        root->addWidget(setupBox);

        auto *buttons = new QHBoxLayout();
        probeBtn = new QPushButton("Probe");
        saveBtn = new QPushButton("Save");
        hookBtn = new QPushButton("Install hook");
        inventoryBtn = new QPushButton("Inventory");
        diagnoseBtn = new QPushButton("Preflight");
        simulateBtn = new QPushButton("Simulate VM");
        vmBtn = new QPushButton("Reboot to VM");
        hostBtn = new QPushButton("Reboot to Host");
        refreshBtn = new QPushButton("Refresh status");
        buttons->addWidget(probeBtn);
        buttons->addWidget(saveBtn);
        buttons->addWidget(hookBtn);
        buttons->addWidget(inventoryBtn);
        buttons->addWidget(diagnoseBtn);
        buttons->addWidget(simulateBtn);
        buttons->addWidget(vmBtn);
        buttons->addWidget(hostBtn);
        buttons->addWidget(refreshBtn);
        root->addLayout(buttons);

        log = new QPlainTextEdit();
        log->setReadOnly(true);
        log->setPlaceholderText("Logs and compatibility checks appear here.");
        root->addWidget(log, 1);

        setCentralWidget(central);
        statusBar()->showMessage("Ready");

        connect(probeBtn, &QPushButton::clicked, this, &MainWindow::onProbe);
        connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSave);
        connect(hookBtn, &QPushButton::clicked, this, &MainWindow::onInstallHook);
        connect(inventoryBtn, &QPushButton::clicked, this, &MainWindow::onInventory);
        connect(diagnoseBtn, &QPushButton::clicked, this, &MainWindow::onDiagnose);
        connect(simulateBtn, &QPushButton::clicked, this, &MainWindow::onSimulateVm);
        connect(vmBtn, &QPushButton::clicked, this, &MainWindow::onSwitchToVm);
        connect(hostBtn, &QPushButton::clicked, this, &MainWindow::onSwitchToHost);
        connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshStatus);

        refreshFromConfig();
        refreshStatus();
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
            {"autoStartVmOnBoot", autoStartVm->isChecked()}
        };
        auto resp = callHelper(req, &err);
        if (!err.isEmpty()) {
            appendLog("Save failed: " + err);
            return;
        }
        appendLog("Config saved.");
        appendLog(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        refreshStatus();
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
        refreshStatus();
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

    void refreshStatus() {
        QString err;
        auto resp = callHelper({{"cmd", "status"}}, &err);
        if (!err.isEmpty()) {
            appendLog("Status unavailable: " + err);
            return;
        }
        const auto inventory = resp.value("inventory").toObject().toVariantMap();
        const auto preflight = resp.value("preflight").toObject().toVariantMap();
        summary->setText(renderSummary(inventory, preflight));
        appendLog("Status:\n" + QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Indented)));
        appendLog(renderInventoryText(inventory));
        appendLog(renderPreflightText(preflight));
    }

private:
    QString renderSummary(const QVariantMap &inventory, const QVariantMap &preflight) const {
        const bool ready = preflight.value("canEnterVm").toBool();
        const auto blockers = preflight.value("blockers").toList();
        const auto warnings = preflight.value("warnings").toList();
        QString text = ready ? "System is ready for VM mode." : "VM mode is blocked.";
        text += " GPUs: " + QString::number(inventory.value("graphicsDevices").toList().size());
        text += " | Mode: " + inventory.value("currentMode").toString();
        text += " | Secure Boot: " + QString(inventory.value("secureBootEnabled").toBool() ? "enabled" : "disabled/unknown");
        text += " | IOMMU: " + QString(preflight.value("compatibility").toMap().value("iommuEnabled").toBool() ? "enabled" : "missing");
        const auto nextBoot = inventory.value("nextBootMode").toString();
        if (!nextBoot.isEmpty()) text += " | Next boot: " + nextBoot;
        if (!blockers.isEmpty()) text += " | Blockers: " + QString::number(blockers.size());
        if (!warnings.isEmpty()) text += " | Warnings: " + QString::number(warnings.size());
        return text;
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
        const int idx = preferredBoot->findText(cfg.preferredBoot);
        if (idx >= 0) preferredBoot->setCurrentIndex(idx);
    }

    void appendLog(const QString &msg) {
        log->appendPlainText(msg);
    }

    QLabel *summary{};
    QLineEdit *vmName{};
    QLineEdit *vmUuid{};
    QLineEdit *gpuBdf{};
    QLineEdit *audioBdf{};
    QComboBox *preferredBoot{};
    QCheckBox *vendorReset{};
    QCheckBox *fallbackDisplay{};
    QCheckBox *allowSingleGpu{};
    QCheckBox *autoStartVm{};
    QPushButton *probeBtn{};
    QPushButton *saveBtn{};
    QPushButton *hookBtn{};
    QPushButton *inventoryBtn{};
    QPushButton *diagnoseBtn{};
    QPushButton *simulateBtn{};
    QPushButton *vmBtn{};
    QPushButton *hostBtn{};
    QPushButton *refreshBtn{};
    QPlainTextEdit *log{};
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    return app.exec();
}

#include "gui_main.moc"
