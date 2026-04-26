#include "common.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QDateTime>
#include <QDebug>

using namespace gs;

static QJsonObject errorReply(const QString &msg) {
    return QJsonObject{{"ok", false}, {"error", msg}};
}

static QJsonObject okReply(const QJsonObject &payload = {}) {
    QJsonObject o = payload;
    o["ok"] = true;
    return o;
}

static void sendJson(QLocalSocket *sock, const QJsonObject &obj) {
    const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    sock->write(line);
    sock->flush();
}

static bool ensureModuleLoaded(const QString &module, QString *detail = nullptr) {
    QProcess p;
    p.start("modprobe", {module});
    if (!p.waitForFinished(7000)) {
        p.kill();
        p.waitForFinished();
        if (detail) *detail = "modprobe " + module + " timed out";
        return false;
    }
    if (detail) {
        *detail = "modprobe " + module + " exit=" + QString::number(p.exitCode());
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}


static bool startVmDomain(const AppConfig &cfg, QStringList *notes, QString *error) {
    const QString domain = domainIdForConfig(cfg);
    if (domain.isEmpty()) {
        if (error) *error = "VM name/UUID is not configured";
        return false;
    }

    int exitCode = 0;
    const QString out = runCommandCapture("virsh", {"-c", "qemu:///system", "start", domain}, 15000, &exitCode);
    const QString lower = out.toLower();
    if (exitCode == 0) {
        if (notes) notes->push_back("Started VM domain " + domain);
        return true;
    }
    if (lower.contains("already active") || lower.contains("already running")) {
        if (notes) notes->push_back("VM domain " + domain + " was already active");
        return true;
    }
    if (error) *error = "Failed to start VM domain " + domain + ": " + out.trimmed();
    return false;
}

static bool scheduleBootTransition(AppConfig cfg, const QString &target, QStringList *notes, QString *error) {
    const QString normalized = normalizeBootTarget(target);
    if (normalized.isEmpty()) {
        if (error) *error = "Invalid boot target: " + target;
        return false;
    }

    cfg.nextBootMode = normalized;
    cfg.vmStoppedAwaitingDecision = false;
    QString saveError;
    if (!saveConfig(cfg, &saveError) || !writeState("pending-" + normalized, &saveError)) {
        if (error) *error = saveError;
        return false;
    }
    if (notes) notes->push_back("Scheduled reboot into " + normalized + " mode");
    return true;
}

static bool markVmStoppedAwaitingDecision(AppConfig cfg, const QString &domain, QStringList *notes, QString *error) {
    if (!cfg.vmName.trimmed().isEmpty() && !domain.trimmed().isEmpty() && domain.trimmed() != cfg.vmName.trimmed()) {
        if (notes) notes->push_back("Ignoring stopped VM " + domain + "; configured VM is " + cfg.vmName.trimmed());
        return true;
    }

    cfg.vmStoppedAwaitingDecision = true;
    cfg.lastStoppedVm = domain.trimmed().isEmpty() ? domainIdForConfig(cfg) : domain.trimmed();
    cfg.lastStoppedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    cfg.nextBootMode.clear();
    cfg.mode = "vm";

    QString saveError;
    if (!saveConfig(cfg, &saveError) || !writeState("vm-stopped-awaiting-decision", &saveError)) {
        if (error) *error = saveError;
        return false;
    }
    if (notes) {
        notes->push_back("VM stopped; waiting for user recovery decision");
        notes->push_back("Options: reboot to host now, restore host on next restart, or keep GPU assigned to VM");
    }
    return true;
}

static bool keepGpuAssignedToVm(AppConfig cfg, QStringList *notes, QString *error) {
    cfg.vmStoppedAwaitingDecision = false;
    cfg.nextBootMode.clear();
    cfg.mode = "vm";
    QString saveError;
    if (!saveConfig(cfg, &saveError) || !writeState("vm", &saveError)) {
        if (error) *error = saveError;
        return false;
    }
    if (notes) notes->push_back("GPU ownership will stay with the VM until changed manually");
    return true;
}

static bool applyBootIntent(QString *error, QStringList *notes);




static QJsonObject buildSimulationReport(const AppConfig &cfg, const QString &flow) {
    const PreflightReport preflight = buildPreflightReport(cfg);
    QJsonObject report;
    report["flow"] = flow;
    report["simulated"] = true;
    report["state"] = preflight.canEnterVm ? "simulated-ready" : "simulated-blocked";
    report["preflight"] = QJsonObject::fromVariantMap(preflightToMap(preflight));
    report["compatibility"] = QJsonObject::fromVariantMap(reportToMap(preflight.compatibility));

    QJsonArray phases;
    const QStringList planned = (flow == "host-to-vm")
        ? QStringList{"validate", "snapshot-libvirt-xml", "prepare-boot-target", "reboot", "apply-boot-intent", "bind-vfio-pci"}
        : QStringList{"validate-host-recovery", "prepare-boot-target", "reboot", "apply-boot-intent", "restore-gpu-driver", "restore-audio-driver"};
    for (const auto &phase : planned) phases.push_back(phase);
    report["phases"] = phases;

    QJsonArray notes;
    for (const auto &n : preflight.notes) notes.push_back(n);
    report["notes"] = notes;

    QJsonArray blockers;
    for (const auto &b : preflight.blockers) blockers.push_back(b);
    report["blockers"] = blockers;

    QJsonArray warnings;
    for (const auto &w : preflight.warnings) warnings.push_back(w);
    report["warnings"] = warnings;
    return report;
}

static bool installHook(QString *error) {
    if (!ensureRuntimeDirs(error)) return false;

    const QString script = R"(#!/usr/bin/env bash
set -euo pipefail
DOMAIN="${1:-}"
OP="${2:-}"
SUBOP="${3:-}"

case "${OP}:${SUBOP}" in
  stopped:end)
    if [[ -x /usr/local/bin/gpu-switcher-ctl ]]; then
      /usr/local/bin/gpu-switcher-ctl on-vm-stopped "${DOMAIN}" || true
    fi
    ;;
  *)
    :
    ;;
esac
)";

    if (!writeFile(hookPath(), script, error)) return false;
    QProcess chmod;
    chmod.start("chmod", {"0755", hookPath()});
    if (!chmod.waitForFinished(5000) || chmod.exitCode() != 0) {
        if (error) *error = "Failed to mark hook executable";
        return false;
    }

    QString detail;
    if (!hookInstalled(&detail)) {
        if (error) *error = "Hook install verification failed: " + detail;
        return false;
    }
    return true;
}

static bool safeBindSequence(const QString &bdf, const QString &targetDriver, QStringList *notes, QString *error) {
    const QString normalized = normalizeBdf(bdf);
    if (!isPciBdf(normalized)) {
        if (error) *error = "Invalid PCI BDF: " + bdf;
        return false;
    }

    if (!setDriverOverride(normalized, targetDriver, error)) return false;
    if (!unbindDriver(normalized, error)) return false;
    if (!bindToDriver(normalized, targetDriver, error)) {
        if (notes) notes->push_back("Bind to " + targetDriver + " failed, trying remove/rescan fallback");
        (void)removeFromBus(normalized, nullptr);
        (void)rescanBus(nullptr);
        if (!bindToDriver(normalized, targetDriver, error)) return false;
    }
    if (notes) notes->push_back("Bound " + normalized + " to " + targetDriver);
    return true;
}

static bool recoverDeviceToDriver(const QString &bdf, const QString &driver, QStringList *notes, QString *error) {
    const QString normalized = normalizeBdf(bdf);
    if (!isPciBdf(normalized)) return true;

    (void)clearDriverOverride(normalized, nullptr);
    (void)unbindDriver(normalized, nullptr);
    if (bindToDriver(normalized, driver, nullptr)) {
        if (notes) notes->push_back("Rebound " + normalized + " to " + driver);
        return true;
    }

    if (notes) notes->push_back("Direct rebind failed for " + normalized + "; attempting remove/rescan");
    if (!removeFromBus(normalized, nullptr) || !rescanBus(nullptr) || !bindToDriver(normalized, driver, error)) {
        if (error && error->isEmpty()) *error = "Failed to recover " + normalized + " to driver " + driver;
        return false;
    }
    if (notes) notes->push_back("Recovered " + normalized + " after remove/rescan");
    return true;
}

class SwitchStateMachine {
public:
    explicit SwitchStateMachine(AppConfig cfg) : cfg_(std::move(cfg)) {}

    bool runVm(QJsonObject *report, QString *error) {
        report_ = QJsonObject();
        phases_.clear();
        notes_.clear();
        report_.insert("flow", "host-to-vm");
        report_.insert("state", "idle");
        report_.insert("phases", QJsonArray{});
        report_.insert("notes", QJsonArray{});

        state_ = VmState::Validate;
        while (state_ != VmState::Done && state_ != VmState::Failed) {
            if (!stepVm(error)) {
                state_ = VmState::Failed;
                break;
            }
        }

        report_["state"] = state_ == VmState::Done ? "done" : "failed";
        report_["phase"] = vmStateName(state_);
        report_["notes"] = notesToArray();
        report_["phases"] = phasesToArray();
        report_["preflight"] = QJsonObject::fromVariantMap(preflightToMap(preflight_));
        report_["compatibility"] = QJsonObject::fromVariantMap(reportToMap(preflight_.compatibility));
        if (report) *report = report_;
        return state_ == VmState::Done;
    }

    bool runHost(QJsonObject *report, QString *error) {
        report_ = QJsonObject();
        phases_.clear();
        notes_.clear();
        report_.insert("flow", "vm-to-host");
        report_.insert("state", "idle");
        report_.insert("phases", QJsonArray{});
        report_.insert("notes", QJsonArray{});

        hostState_ = HostState::Validate;
        while (hostState_ != HostState::Done && hostState_ != HostState::Failed) {
            if (!stepHost(error)) {
                hostState_ = HostState::Failed;
                break;
            }
        }

        report_["state"] = hostState_ == HostState::Done ? "done" : "failed";
        report_["phase"] = hostStateName(hostState_);
        report_["notes"] = notesToArray();
        report_["phases"] = phasesToArray();
        report_["preflight"] = QJsonObject::fromVariantMap(preflightToMap(preflight_));
        report_["compatibility"] = QJsonObject::fromVariantMap(reportToMap(preflight_.compatibility));
        if (report) *report = report_;
        return hostState_ == HostState::Done;
    }

    PreflightReport diagnose() {
        preflight_ = buildPreflightReport(cfg_);
        return preflight_;
    }

    const AppConfig &config() const {
        return cfg_;
    }

private:
    enum class VmState { Validate, SnapshotXml, QuiesceHost, ResetDevice, BindVfio, Persist, Done, Failed };
    enum class HostState { Validate, RestoreGpu, RestoreAudio, Persist, Done, Failed };

    bool stepVm(QString *error) {
        switch (state_) {
        case VmState::Validate: {
            markPhase("validate");
            preflight_ = buildPreflightReport(cfg_);
            if (!preflight_.canEnterVm) {
                if (error) *error = preflight_.blockers.join("; ");
                return false;
            }
            if (!preflight_.compatibility.hookInstalled) {
                if (error) *error = "Libvirt hook is missing or not executable";
                return false;
            }
            if (!preflight_.compatibility.vmXmlMatches) {
                if (error) *error = "Libvirt XML does not match the configured devices";
                return false;
            }
            if (!preflight_.compatibility.gpuExists) {
                if (error) *error = "GPU not found: " + cfg_.gpuBdf;
                return false;
            }
            if (!preflight_.compatibility.groupSafe) {
                if (error) *error = "GPU is in an unsafe IOMMU group";
                return false;
            }
            if (!preflight_.compatibility.iommuEnabled) {
                if (error) *error = "IOMMU is not enabled";
                return false;
            }
            if (preflight_.compatibility.singleGraphicsPath && !cfg_.hasFallbackDisplay && !cfg_.allowSingleGpu) {
                if (error) *error = "Single-GPU path requires fallback display or explicit acknowledgement";
                return false;
            }
            state_ = VmState::SnapshotXml;
            return true;
        }
        case VmState::SnapshotXml: {
            markPhase("snapshot-libvirt-xml");
            if (!backupVmXml(cfg_, &xmlBackup_, error)) return false;
            notes_ << "Backed up inactive domain XML to " + xmlBackup_;
            state_ = VmState::QuiesceHost;
            return true;
        }
        case VmState::QuiesceHost: {
            markPhase("prepare-host-services");
            const DeviceInfo gpu = inspectDevice(cfg_.gpuBdf);
            if (cfg_.originalGpuDriver.isEmpty()) cfg_.originalGpuDriver = gpu.driverName;
            if (!cfg_.audioBdf.isEmpty() && cfg_.originalAudioDriver.isEmpty()) {
                cfg_.originalAudioDriver = inspectDevice(cfg_.audioBdf).driverName;
            }
            if (gpu.vendorId.isEmpty()) {
                if (error) *error = "Unable to inspect GPU vendor data";
                return false;
            }
            const VendorKind kind = vendorKind(gpu);
            notes_ << "Detected vendor: " + vendorKindName(kind);
            if (kind == VendorKind::AMD && cfg_.useVendorReset) {
                QString moduleDetail;
                if (ensureModuleLoaded("vendor-reset", &moduleDetail)) {
                    notes_ << moduleDetail;
                } else {
                    notes_ << "vendor-reset requested but not available";
                }
            }
            state_ = VmState::ResetDevice;
            return true;
        }
        case VmState::ResetDevice: {
            markPhase("reset-gpu");
            QString resetError;
            if (!bestEffortReset(cfg_.gpuBdf, cfg_, &notes_, &resetError)) {
                notes_ << resetError;
                notes_ << "Continuing with VFIO binding; reboot fallback may still be required later";
            }
            state_ = VmState::BindVfio;
            return true;
        }
        case VmState::BindVfio: {
            markPhase("bind-vfio-pci");
            QString bindError;
            if (!ensureModuleLoaded("vfio-pci")) {
                if (error) *error = "Could not load vfio-pci";
                return false;
            }
            if (!safeBindSequence(cfg_.gpuBdf, "vfio-pci", &notes_, &bindError)) {
                if (error) *error = bindError;
                return false;
            }
            if (!cfg_.audioBdf.isEmpty()) {
                if (!safeBindSequence(cfg_.audioBdf, "vfio-pci", &notes_, nullptr)) {
                    notes_ << "Audio function could not be bound to vfio-pci; GPU passthrough may still work";
                }
            }
            state_ = VmState::Persist;
            return true;
        }
        case VmState::Persist: {
            markPhase("persist-state");
            cfg_.mode = "vm";
            cfg_.nextBootMode.clear();
            QString saveError;
            if (!saveConfig(cfg_, &saveError) || !writeState("vm", &saveError)) {
                if (error) *error = saveError;
                return false;
            }
            notes_ << "State persisted as vm";
            state_ = VmState::Done;
            return true;
        }
        case VmState::Done:
        case VmState::Failed:
            return true;
        }
        return false;
    }

    bool stepHost(QString *error) {
        switch (hostState_) {
        case HostState::Validate: {
            markPhase("validate-host-recovery");
            preflight_ = buildPreflightReport(cfg_);
            hostState_ = HostState::RestoreGpu;
            return true;
        }
        case HostState::RestoreGpu: {
            markPhase("restore-gpu-driver");
            if (!cfg_.gpuBdf.isEmpty()) {
                if (cfg_.originalGpuDriver.isEmpty()) {
                    if (error) *error = "Original GPU driver is unknown";
                    return false;
                }
                if (!recoverDeviceToDriver(cfg_.gpuBdf, cfg_.originalGpuDriver, &notes_, error)) {
                    return false;
                }
            }
            hostState_ = HostState::RestoreAudio;
            return true;
        }
        case HostState::RestoreAudio: {
            markPhase("restore-audio-driver");
            if (!cfg_.audioBdf.isEmpty()) {
                if (cfg_.originalAudioDriver.isEmpty()) {
                    if (error) *error = "Original audio driver is unknown";
                    return false;
                }
                if (!recoverDeviceToDriver(cfg_.audioBdf, cfg_.originalAudioDriver, &notes_, error)) {
                    return false;
                }
            }
            hostState_ = HostState::Persist;
            return true;
        }
        case HostState::Persist: {
            markPhase("persist-host-state");
            cfg_.mode = "host";
            cfg_.nextBootMode.clear();
            QString saveError;
            if (!saveConfig(cfg_, &saveError) || !writeState("host", &saveError)) {
                if (error) *error = saveError;
                return false;
            }
            notes_ << "State persisted as host";
            hostState_ = HostState::Done;
            return true;
        }
        case HostState::Done:
        case HostState::Failed:
            return true;
        }
        return false;
    }

    void markPhase(const QString &phase) {
        phases_ << phase;
        report_["phase"] = phase;
    }

    QJsonArray notesToArray() const {
        QJsonArray arr;
        for (const auto &n : notes_) arr.push_back(n);
        return arr;
    }

    QJsonArray phasesToArray() const {
        QJsonArray arr;
        for (const auto &p : phases_) arr.push_back(p);
        return arr;
    }

    static QString vmStateName(VmState s) {
        switch (s) {
        case VmState::Validate: return "validate";
        case VmState::SnapshotXml: return "snapshot-xml";
        case VmState::QuiesceHost: return "quiesce-host";
        case VmState::ResetDevice: return "reset-device";
        case VmState::BindVfio: return "bind-vfio";
        case VmState::Persist: return "persist";
        case VmState::Done: return "done";
        case VmState::Failed: return "failed";
        }
        return "unknown";
    }

    static QString hostStateName(HostState s) {
        switch (s) {
        case HostState::Validate: return "validate";
        case HostState::RestoreGpu: return "restore-gpu";
        case HostState::RestoreAudio: return "restore-audio";
        case HostState::Persist: return "persist";
        case HostState::Done: return "done";
        case HostState::Failed: return "failed";
        }
        return "unknown";
    }

    AppConfig cfg_;
    QJsonObject report_;
    QString xmlBackup_;
    PreflightReport preflight_;
    QStringList phases_;
    QStringList notes_;
    VmState state_ = VmState::Validate;
    HostState hostState_ = HostState::Validate;
};
static bool applyBootIntent(QString *error, QStringList *notes) {
    AppConfig cfg = loadConfig(error);
    if (error && !error->isEmpty()) {
        return false;
    }

    const QString target = normalizeBootTarget(cfg.nextBootMode);
    if (target.isEmpty()) {
        if (notes) notes->push_back("No pending boot target; nothing to apply");
        return true;
    }

    SwitchStateMachine sm(cfg);
    QJsonObject report;
    bool ok = false;
    if (target == "vm") {
        ok = sm.runVm(&report, error);
    } else {
        ok = sm.runHost(&report, error);
    }
    if (!ok) {
        if (notes) notes->push_back("Boot intent application failed: " + (error ? *error : QString()));
        return false;
    }

    cfg = sm.config();
    cfg.nextBootMode.clear();
    cfg.vmStoppedAwaitingDecision = false;
    cfg.mode = target;
    QString saveError;
    if (!saveConfig(cfg, &saveError) || !writeState(target, &saveError)) {
        if (error) *error = saveError;
        return false;
    }
    if (notes) {
        notes->push_back("Applied boot intent: " + target);
        notes->push_back("Host/GPU state persisted as " + target);
    }

    if (target == "vm" && cfg.autoStartVmOnBoot && !cfg.vmName.trimmed().isEmpty()) {
        if (!startVmDomain(cfg, notes, error)) {
            return false;
        }
    }
    return true;
}


class HelperServer : public QObject {
    Q_OBJECT
public:
    explicit HelperServer(QObject *parent = nullptr) : QObject(parent) {}

    bool start(QString *error) {
        QLocalServer::removeServer(socketPath());
        server.setSocketOptions(QLocalServer::WorldAccessOption);
        if (!server.listen(socketPath())) {
            if (error) *error = server.errorString();
            return false;
        }
        connect(&server, &QLocalServer::newConnection, this, &HelperServer::onNewConnection);
        return true;
    }

private slots:
    void onNewConnection() {
        while (server.hasPendingConnections()) {
            auto *sock = server.nextPendingConnection();
            connect(sock, &QLocalSocket::readyRead, this, [this, sock]() { handleSocket(sock); });
            connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
        }
    }

private:
    void handleSocket(QLocalSocket *sock) {
        while (sock->canReadLine()) {
            const QByteArray line = sock->readLine().trimmed();
            if (line.isEmpty()) continue;

            const auto doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) {
                sendJson(sock, errorReply("Invalid JSON"));
                continue;
            }

            const QJsonObject req = doc.object();
            const QString cmd = req.value("cmd").toString();

            if (cmd == "probe") {
                const QString bdf = normalizeBdf(req.value("gpuBdf").toString());
                QJsonObject out;
                out["device"] = QJsonObject::fromVariantMap(deviceInfoToMap(inspectDevice(bdf)));
                AppConfig cfg = loadConfig();
                cfg.gpuBdf = bdf;
                out["compatibility"] = QJsonObject::fromVariantMap(reportToMap(buildCompatibilityReport(cfg)));
                out["preflight"] = QJsonObject::fromVariantMap(preflightToMap(buildPreflightReport(cfg)));
                out["state"] = readState();
                sendJson(sock, okReply(out));
                continue;
            }

            if (cmd == "status") {
                const AppConfig cfg = loadConfig();
                QJsonObject out;
                out["state"] = readState();
                out["compatibility"] = QJsonObject::fromVariantMap(reportToMap(buildCompatibilityReport(cfg)));
                out["preflight"] = QJsonObject::fromVariantMap(preflightToMap(buildPreflightReport(cfg)));
                out["inventory"] = QJsonObject::fromVariantMap(systemInventoryToMap());
                out["hookPath"] = hookPath();
                out["hookInstalled"] = hookInstalled();
                out["config"] = QJsonObject{
                    {"vmName", cfg.vmName},
                    {"vmUuid", cfg.vmUuid},
                    {"gpuBdf", cfg.gpuBdf},
                    {"audioBdf", cfg.audioBdf},
                    {"preferredBoot", cfg.preferredBoot},
                    {"nextBootMode", cfg.nextBootMode},
                    {"useVendorReset", cfg.useVendorReset},
                    {"hasFallbackDisplay", cfg.hasFallbackDisplay},
                    {"allowSingleGpu", cfg.allowSingleGpu},
                    {"autoStartVmOnBoot", cfg.autoStartVmOnBoot},
                    {"vmStoppedAwaitingDecision", cfg.vmStoppedAwaitingDecision},
                    {"lastStoppedVm", cfg.lastStoppedVm},
                    {"lastStoppedAt", cfg.lastStoppedAt},
                    {"mode", cfg.mode},
                    {"originalGpuDriver", cfg.originalGpuDriver},
                    {"originalAudioDriver", cfg.originalAudioDriver},
                };
                sendJson(sock, okReply(out));
                continue;
            }

            if (cmd == "inventory") {
                sendJson(sock, okReply({{"inventory", QJsonObject::fromVariantMap(systemInventoryToMap())}}));
                continue;
            }

            if (cmd == "diagnose") {
                AppConfig cfg = loadConfig();
                const QString bdf = normalizeBdf(req.value("gpuBdf").toString());
                if (!bdf.isEmpty()) cfg.gpuBdf = bdf;
                const auto diag = buildPreflightReport(cfg);
                sendJson(sock, okReply({{"diagnostic", QJsonObject::fromVariantMap(preflightToMap(diag))}}));
                continue;
            }

            if (cmd == "simulateVm" || cmd == "simulateHost" || cmd == "simulate") {
                AppConfig cfg = loadConfig();
                const QString bdf = normalizeBdf(req.value("gpuBdf").toString());
                if (!bdf.isEmpty()) cfg.gpuBdf = bdf;
                const QString flow = (cmd == "simulateHost") ? "vm-to-host" : "host-to-vm";
                sendJson(sock, okReply({{"simulation", buildSimulationReport(cfg, flow)}}));
                continue;
            }

            if (cmd == "saveConfig") {
                AppConfig cfg = loadConfig();
                cfg.vmName = req.value("vmName").toString().trimmed();
                cfg.vmUuid = req.value("vmUuid").toString().trimmed();
                cfg.gpuBdf = normalizeBdf(req.value("gpuBdf").toString());
                cfg.audioBdf = normalizeBdf(req.value("audioBdf").toString());
                cfg.preferredBoot = req.value("preferredBoot").toString(cfg.preferredBoot.isEmpty() ? "host" : cfg.preferredBoot);
                cfg.nextBootMode = normalizeBootTarget(req.value("nextBootMode").toString(cfg.nextBootMode));
                cfg.useVendorReset = req.value("useVendorReset").toBool(cfg.useVendorReset);
                cfg.hasFallbackDisplay = req.value("hasFallbackDisplay").toBool(cfg.hasFallbackDisplay);
                cfg.allowSingleGpu = req.value("allowSingleGpu").toBool(cfg.allowSingleGpu);
                cfg.autoStartVmOnBoot = req.value("autoStartVmOnBoot").toBool(cfg.autoStartVmOnBoot);
                cfg.vmStoppedAwaitingDecision = req.value("vmStoppedAwaitingDecision").toBool(cfg.vmStoppedAwaitingDecision);
                cfg.lastStoppedVm = req.value("lastStoppedVm").toString(cfg.lastStoppedVm);
                cfg.lastStoppedAt = req.value("lastStoppedAt").toString(cfg.lastStoppedAt);
                cfg.mode = req.value("mode").toString(cfg.mode.isEmpty() ? "host" : cfg.mode);
                cfg.originalGpuDriver = req.value("originalGpuDriver").toString(cfg.originalGpuDriver);
                cfg.originalAudioDriver = req.value("originalAudioDriver").toString(cfg.originalAudioDriver);
                QString err;
                if (!saveConfig(cfg, &err) || !writeState(cfg.mode, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                sendJson(sock, okReply({{"saved", true}}));
                continue;
            }

            if (cmd == "installHook") {
                QString err;
                if (!installHook(&err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                sendJson(sock, okReply({{"hookInstalled", true}, {"path", hookPath()}}));
                continue;
            }

            if (cmd == "switchToVm" || cmd == "rebootToVm") {
                QString err;
                QStringList notes;
                AppConfig cfg = loadConfig();
                if (!scheduleBootTransition(cfg, "vm", &notes, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                sendJson(sock, okReply({{"mode", "pending-vm"}, {"notes", QJsonArray::fromStringList(notes)}}));
                QTimer::singleShot(1500, []() { QProcess::startDetached("systemctl", {"reboot"}); });
                continue;
            }

            if (cmd == "switchToHost" || cmd == "rebootToHost") {
                QString err;
                QStringList notes;
                AppConfig cfg = loadConfig();
                if (!scheduleBootTransition(cfg, "host", &notes, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                sendJson(sock, okReply({{"mode", "pending-host"}, {"notes", QJsonArray::fromStringList(notes)}}));
                QTimer::singleShot(1500, []() { QProcess::startDetached("systemctl", {"reboot"}); });
                continue;
            }

            if (cmd == "applyBootState") {
                QString err;
                QStringList notes;
                if (!applyBootIntent(&err, &notes)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                sendJson(sock, okReply({{"mode", "applied"}, {"notes", QJsonArray::fromStringList(notes)}}));
                continue;
            }

            if (cmd == "returnHostNextRestart" || cmd == "returnHostNextBoot") {
                QString err;
                QStringList notes;
                AppConfig cfg = loadConfig();
                if (!scheduleBootTransition(cfg, "host", &notes, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                notes << "Host recovery will be applied on the next restart; no reboot was triggered now";
                sendJson(sock, okReply({{"mode", "pending-host"}, {"action", "host-on-next-restart"}, {"notes", QJsonArray::fromStringList(notes)}}));
                continue;
            }

            if (cmd == "restartHostNow" || cmd == "vmStoppedRestartHost") {
                QString err;
                QStringList notes;
                AppConfig cfg = loadConfig();
                if (!scheduleBootTransition(cfg, "host", &notes, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                notes << "Host recovery scheduled; reboot will start now";
                sendJson(sock, okReply({{"mode", "pending-host"}, {"action", "reboot-now"}, {"notes", QJsonArray::fromStringList(notes)}}));
                QTimer::singleShot(1500, []() { QProcess::startDetached("systemctl", {"reboot"}); });
                continue;
            }

            if (cmd == "keepGpuForVm" || cmd == "vmStoppedKeepVm") {
                QString err;
                QStringList notes;
                AppConfig cfg = loadConfig();
                if (!keepGpuAssignedToVm(cfg, &notes, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                sendJson(sock, okReply({{"mode", "vm"}, {"action", "keep-vm"}, {"notes", QJsonArray::fromStringList(notes)}}));
                continue;
            }

            if (cmd == "onVmStopped") {
                QString err;
                QStringList notes;
                AppConfig cfg = loadConfig();
                const QString domain = req.value("domain").toString();
                if (!markVmStoppedAwaitingDecision(cfg, domain, &notes, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                sendJson(sock, okReply({{"mode", "vm-stopped-awaiting-decision"}, {"action", "await-user-choice"}, {"domain", domain}, {"notes", QJsonArray::fromStringList(notes)}}));
                continue;
            }

            sendJson(sock, errorReply("Unknown command: " + cmd));
        }
    }

    QLocalServer server;
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.contains("--apply-boot-state")) {
        QString err;
        QStringList notes;
        if (!ensureRuntimeDirs(&err)) {
            qCritical() << err;
            return 1;
        }
        if (!applyBootIntent(&err, &notes)) {
            qCritical() << "Failed to apply boot intent:" << err;
            for (const auto &n : notes) qInfo().noquote() << n;
            return 2;
        }
        for (const auto &n : notes) qInfo().noquote() << n;
        return 0;
    }

    QString err;
    if (!ensureRuntimeDirs(&err)) {
        qCritical() << err;
        return 1;
    }

    HelperServer s;
    if (!s.start(&err)) {
        qCritical() << "Failed to start helper:" << err;
        return 2;
    }

    qInfo() << "gpu-switcher-helperd listening on" << socketPath();
    return app.exec();
}

#include "helperd_main.moc"
