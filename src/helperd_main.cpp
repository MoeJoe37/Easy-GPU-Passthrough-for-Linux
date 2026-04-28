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
#include <QDir>
#include <QRegularExpression>

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

static bool installHook(QString *error);
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



static void runBestEffort(const QString &program, const QStringList &args, QStringList *notes, int timeoutMs = 7000) {
    int exitCode = 0;
    const QString out = runCommandCapture(program, args, timeoutMs, &exitCode).trimmed();
    if (notes) {
        QString msg = program + " " + args.join(' ') + " exit=" + QString::number(exitCode);
        if (!out.isEmpty()) msg += " | " + out.left(240);
        notes->push_back(msg);
    }
}


static void cancelSafetyRecovery(QStringList *notes) {
    runBestEffort("systemctl", {"stop", "gpu-switcher-safety-recover.timer", "gpu-switcher-safety-recover.service"}, notes, 5000);
    runBestEffort("systemctl", {"reset-failed", "gpu-switcher-safety-recover.timer", "gpu-switcher-safety-recover.service"}, notes, 5000);
}

static void scheduleSafetyRecoveryTimer(const AppConfig &cfg, QStringList *notes) {
    if (cfg.safetyAutoRecoveryMinutes <= 0) {
        if (notes) notes->push_back("Safety auto recovery is disabled by config");
        return;
    }

    cancelSafetyRecovery(notes);

    const QString delay = QString::number(cfg.safetyAutoRecoveryMinutes) + "min";
    runBestEffort("systemd-run",
                  {"--unit=gpu-switcher-safety-recover",
                   "--collect",
                   "--on-active=" + delay,
                   "/usr/local/bin/gpu-switcher-ctl",
                   "safetyRecoverHostNow"},
                  notes,
                  10000);
    if (notes) notes->push_back("Safety recovery timer armed for " + delay + " after VM stop");
}

static void stopDisplayManager(QStringList *notes) {
    runBestEffort("systemctl", {"stop", "display-manager.service"}, notes, 10000);
}

static void unmaskDisplayManager(QStringList *notes) {
    runBestEffort("systemctl", {"unmask", "display-manager.service"}, notes, 10000);
}

static void maskDisplayManagerForThisBoot(QStringList *notes) {
    // Runtime mask disappears on reboot; it prevents the Linux login manager
    // from racing the VM for the only GPU during VM-mode boots.
    runBestEffort("systemctl", {"mask", "--runtime", "display-manager.service"}, notes, 10000);
}

static void bindVtConsoles(bool bind, QStringList *notes) {
    QDir dir("/sys/class/vtconsole");
    const auto entries = dir.entryList(QStringList{"vtcon*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        QFile f(dir.absoluteFilePath(entry + "/bind"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(bind ? "1" : "0");
            if (notes) notes->push_back(QString("%1 VT console %2").arg(bind ? "Rebound" : "Unbound", entry));
        }
    }
}

static void unbindPlatformFramebuffers(QStringList *notes) {
    const QStringList drivers = {"efi-framebuffer", "simple-framebuffer", "vesafb", "simpledrm"};
    for (const auto &driver : drivers) {
        QDir d("/sys/bus/platform/drivers/" + driver);
        if (!d.exists()) continue;
        const auto devices = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto &dev : devices) {
            QFile unbind(d.absoluteFilePath("unbind"));
            if (unbind.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                unbind.write(dev.toUtf8());
                if (notes) notes->push_back("Unbound platform framebuffer " + driver + ":" + dev);
            }
        }
    }
}

static void unloadGpuModulesFor(VendorKind kind, QStringList *notes) {
    if (kind == VendorKind::NVIDIA) {
        runBestEffort("modprobe", {"-r", "nvidia_drm", "nvidia_modeset", "nvidia_uvm", "nvidia", "i2c_nvidia_gpu"}, notes, 10000);
    } else if (kind == VendorKind::AMD) {
        runBestEffort("modprobe", {"-r", "amdgpu", "radeon"}, notes, 10000);
    } else if (kind == VendorKind::Intel) {
        runBestEffort("modprobe", {"-r", "i915", "xe"}, notes, 10000);
    }
}

static void loadGpuModulesForDriver(const QString &driver, QStringList *notes) {
    const QString d = driver.trimmed();
    if (d.isEmpty() || d == "vfio-pci") return;
    if (d == "nvidia" || d.startsWith("nvidia_")) {
        runBestEffort("modprobe", {"nvidia"}, notes, 10000);
        runBestEffort("modprobe", {"nvidia_modeset"}, notes, 10000);
        runBestEffort("modprobe", {"nvidia_uvm"}, notes, 10000);
        runBestEffort("modprobe", {"nvidia_drm"}, notes, 10000);
    } else {
        runBestEffort("modprobe", {d}, notes, 10000);
    }
}

static void prepareSingleGpuHostForVfio(bool singleGraphicsPath, VendorKind kind, QStringList *notes) {
    if (!singleGraphicsPath) return;
    if (notes) notes->push_back("Single-GPU transition: preventing display-manager/framebuffer from racing VFIO");
    maskDisplayManagerForThisBoot(notes);
    stopDisplayManager(notes);
    bindVtConsoles(false, notes);
    unbindPlatformFramebuffers(notes);
    unloadGpuModulesFor(kind, notes);
}

static void restoreHostConsoleAfterRecovery(QStringList *notes) {
    bindVtConsoles(true, notes);
    unmaskDisplayManager(notes);
}

static QString hostdevXmlForBdf(const QString &bdf) {
    const QString n = normalizeBdf(bdf);
    const QString domain = n.section(':', 0, 0);
    const QString bus = n.section(':', 1, 1);
    const QString devFunc = n.section(':', 2, 2);
    const QString slot = devFunc.section('.', 0, 0);
    const QString function = devFunc.section('.', 1, 1);
    return QString(
        "    <hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "      <driver name='vfio'/>\n"
        "      <source>\n"
        "        <address domain='0x%1' bus='0x%2' slot='0x%3' function='0x%4'/>\n"
        "      </source>\n"
        "    </hostdev>\n")
        .arg(domain, bus, slot, function);
}

static bool xmlContainsBdfHostdev(const QString &xml, const QString &bdf) {
    const QString n = normalizeBdf(bdf);
    if (n.isEmpty()) return false;
    const QString domain = n.section(':', 0, 0);
    const QString bus = n.section(':', 1, 1);
    const QString devFunc = n.section(':', 2, 2);
    const QString slot = devFunc.section('.', 0, 0);
    const QString function = devFunc.section('.', 1, 1);
    QRegularExpression re(QString("<address[^>]+domain=['\\\"]0x%1['\\\"][^>]+bus=['\\\"]0x%2['\\\"][^>]+slot=['\\\"]0x%3['\\\"][^>]+function=['\\\"]0x%4['\\\"]")
                          .arg(domain, bus, slot, function), QRegularExpression::CaseInsensitiveOption);
    return re.match(xml).hasMatch();
}

static bool backupRawDomainXml(const QString &domain, const QString &xml, QString *backupPath, QString *error) {
    if (!ensureRuntimeDirs(error)) return false;
    QString safeDomain = domain;
    safeDomain.replace('/', '_');
    const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
    const QString path = backupDirPath() + "/" + safeDomain + "-before-autopatch-" + stamp + ".xml";
    if (!writeFile(path, xml, error)) return false;
    if (backupPath) *backupPath = path;
    return true;
}

static bool ensureDomainHasHostdevs(const AppConfig &cfg, QStringList *notes, QString *error) {
    const QString domain = domainIdForConfig(cfg);
    if (domain.isEmpty()) {
        if (error) *error = "VM name/UUID is not configured";
        return false;
    }

    int exitCode = 0;
    QString xml = runCommandCapture("virsh", {"-c", "qemu:///system", "dumpxml", "--inactive", domain}, 10000, &exitCode);
    if (exitCode != 0 || xml.trimmed().isEmpty()) {
        if (error) *error = "Unable to read inactive XML for " + domain;
        return false;
    }

    QString backup;
    if (!backupRawDomainXml(domain, xml, &backup, error)) return false;
    if (notes) notes->push_back("Backed up inactive VM XML to " + backup);

    QString insertion;
    const QStringList wanted = cfg.audioBdf.trimmed().isEmpty()
        ? QStringList{normalizeBdf(cfg.gpuBdf)}
        : QStringList{normalizeBdf(cfg.gpuBdf), normalizeBdf(cfg.audioBdf)};
    for (const auto &bdf : wanted) {
        if (bdf.isEmpty()) continue;
        if (!xmlContainsBdfHostdev(xml, bdf)) {
            insertion += hostdevXmlForBdf(bdf);
            if (notes) notes->push_back("Adding PCI hostdev to VM XML: " + bdf);
        } else if (notes) {
            notes->push_back("VM XML already contains PCI hostdev: " + bdf);
        }
    }

    if (insertion.isEmpty()) return true;
    const int pos = xml.indexOf("</devices>");
    if (pos < 0) {
        if (error) *error = "Inactive VM XML has no </devices> section";
        return false;
    }
    xml.insert(pos, insertion);

    const QString patchedPath = backupDirPath() + "/" + domain + "-autopatched.xml";
    if (!writeFile(patchedPath, xml, error)) return false;
    const QString defineOut = runCommandCapture("virsh", {"-c", "qemu:///system", "define", patchedPath}, 15000, &exitCode);
    if (exitCode != 0) {
        if (error) *error = "virsh define failed after XML autopatch: " + defineOut.trimmed();
        return false;
    }
    if (notes) notes->push_back("Defined autopatched VM XML with GPU/audio hostdevs");
    return true;
}

static QString chooseBestPassthroughGpu(QStringList *notes) {
    const QStringList gfx = allGraphicsControllers(nullptr);
    if (gfx.isEmpty()) return {};
    if (gfx.size() == 1) return normalizeBdf(gfx.first());

    QString firstDiscrete;
    QString firstHybridSafe;
    for (const auto &raw : gfx) {
        const QString bdf = normalizeBdf(raw);
        const DeviceInfo d = inspectDevice(bdf);
        const VendorKind kind = vendorKind(d);
        if (kind == VendorKind::Intel) continue;
        if (firstDiscrete.isEmpty()) firstDiscrete = bdf;
        const LaptopSafetyInfo laptop = inspectLaptopSafety(bdf);
        if (laptop.likelyLaptop && laptop.targetAppearsSafeHybridOffload) {
            firstHybridSafe = bdf;
            break;
        }
    }

    if (!firstHybridSafe.isEmpty()) {
        if (notes) notes->push_back("Auto-selected hybrid/offload dGPU " + firstHybridSafe);
        return firstHybridSafe;
    }
    if (!firstDiscrete.isEmpty()) {
        if (notes) notes->push_back("Auto-selected non-Intel GPU " + firstDiscrete);
        return firstDiscrete;
    }
    return {};
}

static bool autoConfigureSingleGpu(AppConfig cfg, const QJsonObject &req, QJsonObject *payload, QString *error) {
    QStringList notes;

    const QString reqVmName = req.value("vmName").toString().trimmed();
    const QString reqVmUuid = req.value("vmUuid").toString().trimmed();
    if (!reqVmName.isEmpty()) cfg.vmName = reqVmName;
    if (!reqVmUuid.isEmpty()) cfg.vmUuid = reqVmUuid;

    cfg.useVendorReset = req.value("useVendorReset").toBool(cfg.useVendorReset);
    cfg.hasFallbackDisplay = req.value("hasFallbackDisplay").toBool(cfg.hasFallbackDisplay);
    cfg.allowSingleGpu = req.value("allowSingleGpu").toBool(cfg.allowSingleGpu);
    cfg.autoStartVmOnBoot = req.value("autoStartVmOnBoot").toBool(cfg.autoStartVmOnBoot);
    cfg.thermalGuardEnabled = req.value("thermalGuardEnabled").toBool(cfg.thermalGuardEnabled);
    cfg.maxGpuTempC = req.value("maxGpuTempC").toInt(cfg.maxGpuTempC);
    cfg.safetyAutoRecoveryMinutes = req.value("safetyAutoRecoveryMinutes").toInt(cfg.safetyAutoRecoveryMinutes);

    QString selectedGpu = normalizeBdf(req.value("gpuBdf").toString());
    if (selectedGpu.isEmpty()) selectedGpu = normalizeBdf(cfg.gpuBdf);
    if (selectedGpu.isEmpty()) selectedGpu = chooseBestPassthroughGpu(&notes);
    if (selectedGpu.isEmpty()) {
        const QStringList gfx = allGraphicsControllers(nullptr);
        if (error) *error = "Auto setup could not safely select a passthrough GPU from " + QString::number(gfx.size()) + " graphics devices; select the GPU PCI BDF manually first";
        return false;
    }

    cfg.gpuBdf = selectedGpu;
    QString selectedAudio = normalizeBdf(req.value("audioBdf").toString());
    if (selectedAudio.isEmpty()) selectedAudio = normalizeBdf(cfg.audioBdf);
    if (selectedAudio.isEmpty()) {
        const auto companions = companionAudioFunctions(selectedGpu, nullptr);
        if (!companions.isEmpty()) selectedAudio = companions.first();
    }
    cfg.audioBdf = selectedAudio;

    if (domainIdForConfig(cfg).isEmpty()) {
        if (error) *error = "Set the VM name or UUID before running auto setup";
        return false;
    }

    const LaptopSafetyInfo laptop = inspectLaptopSafety(selectedGpu);
    const bool isSingle = singleGPUOnly(selectedGpu, nullptr);
    const bool laptopDisplayOwner = laptop.likelyLaptop && laptop.targetDrivesActiveDisplay;

    if (cfg.safetyAutoRecoveryMinutes <= 0) {
        cfg.safetyAutoRecoveryMinutes = 10;
        notes << "Enabled VM-stop safety recovery timer at 10 minutes";
    }
    if (!cfg.thermalGuardEnabled) {
        cfg.thermalGuardEnabled = true;
        notes << "Enabled thermal guard during auto setup";
    }

    if (laptop.likelyLaptop) {
        notes << "Laptop compatibility: " + laptop.summary;
        if (!laptop.blockers.isEmpty()) notes << "Laptop topology blockers/warnings: " + laptop.blockers.join("; ");
        if (!laptop.warnings.isEmpty()) notes << "Laptop topology warnings: " + laptop.warnings.join("; ");

        if (!laptop.targetHasDrmCard) {
            if (error) *error = "Laptop compatibility blocked: the target GPU could not be mapped to a DRM/KMS card, so the app cannot verify whether it drives the internal panel";
            return false;
        }
        if (laptopDisplayOwner && !cfg.allowSingleGpu) {
            if (error) *error = "Laptop compatibility blocked: the target GPU currently owns an active display connector. Enable the single-GPU acknowledgement and confirm SSH/fallback recovery before auto setup.";
            return false;
        }
        if (laptopDisplayOwner && !cfg.hasFallbackDisplay && !sshdActive(nullptr)) {
            if (error) *error = "Laptop compatibility blocked: the target GPU owns a display and no SSH/fallback display is active or confirmed.";
            return false;
        }
        if (!laptopDisplayOwner && laptop.targetAppearsSafeHybridOffload) {
            cfg.hasFallbackDisplay = true;
            notes << "Hybrid laptop mode accepted: Linux appears to keep display ownership on another GPU";
        }
    } else if (isSingle) {
        cfg.allowSingleGpu = true;
        notes << "Desktop/single-GPU path accepted by auto setup";
    }

    cfg.autoStartVmOnBoot = true;
    cfg.mode = cfg.mode.isEmpty() ? "host" : cfg.mode;

    if (!saveConfig(cfg, error)) return false;
    notes << "Saved passthrough config: GPU=" + cfg.gpuBdf + (cfg.audioBdf.isEmpty() ? QString() : " audio=" + cfg.audioBdf);

    if (!installHook(error)) return false;
    notes << "Installed libvirt QEMU lifecycle hook";

    if (!ensureDomainHasHostdevs(cfg, &notes, error)) return false;

    QStringList xmlIssues;
    if (!vmXmlMatchesConfig(cfg, &xmlIssues, nullptr)) {
        if (error) *error = "VM XML still does not match after autopatch: " + xmlIssues.join("; ");
        return false;
    }

    const auto diag = buildPreflightReport(cfg);
    if (!diag.canEnterVm) {
        if (error) *error = "Auto setup completed file changes but final safety preflight still blocks VM mode: " + diag.blockers.join("; ");
        return false;
    }

    if (payload) {
        (*payload)["configured"] = true;
        (*payload)["config"] = QJsonObject{
            {"vmName", cfg.vmName}, {"vmUuid", cfg.vmUuid}, {"gpuBdf", cfg.gpuBdf}, {"audioBdf", cfg.audioBdf},
            {"allowSingleGpu", cfg.allowSingleGpu}, {"hasFallbackDisplay", cfg.hasFallbackDisplay}, {"autoStartVmOnBoot", cfg.autoStartVmOnBoot},
            {"thermalGuardEnabled", cfg.thermalGuardEnabled}, {"safetyAutoRecoveryMinutes", cfg.safetyAutoRecoveryMinutes}
        };
        (*payload)["notes"] = QJsonArray::fromStringList(notes);
        (*payload)["diagnostic"] = QJsonObject::fromVariantMap(preflightToMap(diag));
        (*payload)["inventory"] = QJsonObject::fromVariantMap(systemInventoryToMap());
    }
    return true;
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

    if (normalized == "vm") {
        const PreflightReport preflight = buildPreflightReport(cfg);
        if (!preflight.canEnterVm) {
            if (error) *error = "VM boot blocked by safety preflight: " + preflight.blockers.join("; ");
            return false;
        }
        if (notes) notes->push_back("Safety preflight passed before scheduling VM boot");
    }

    cfg.nextBootMode = normalized;
    cfg.safetyRecoveryDeadline.clear();
    cfg.vmStoppedAwaitingDecision = false;
    cancelSafetyRecovery(notes);
    QString saveError;
    if (!saveConfig(cfg, &saveError) || !writeState("pending-" + normalized, &saveError)) {
        if (error) *error = saveError;
        return false;
    }
    if (notes) notes->push_back("Scheduled reboot into " + normalized + " mode");
    return true;
}

static bool markVmStoppedAwaitingDecision(AppConfig cfg, const QString &domain, QStringList *notes, QString *error) {
    const QString d = domain.trimmed();
    const QString configuredName = cfg.vmName.trimmed();
    const QString configuredUuid = cfg.vmUuid.trimmed();
    const bool hasFilter = !configuredName.isEmpty() || !configuredUuid.isEmpty();
    const bool matches = d.isEmpty() || d == configuredName || d == configuredUuid;
    if (hasFilter && !matches) {
        if (notes) notes->push_back("Ignoring stopped VM " + domain + "; configured VM is " + domainIdForConfig(cfg));
        return true;
    }

    cfg.vmStoppedAwaitingDecision = true;
    cfg.lastStoppedVm = domain.trimmed().isEmpty() ? domainIdForConfig(cfg) : domain.trimmed();
    cfg.lastStoppedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    cfg.nextBootMode = "host";
    cfg.mode = "vm";
    if (cfg.safetyAutoRecoveryMinutes > 0) {
        cfg.safetyRecoveryDeadline = QDateTime::currentDateTimeUtc().addSecs(cfg.safetyAutoRecoveryMinutes * 60).toString(Qt::ISODate);
    } else {
        cfg.safetyRecoveryDeadline.clear();
    }

    QString saveError;
    if (!saveConfig(cfg, &saveError) || !writeState("vm-stopped-awaiting-decision", &saveError)) {
        if (error) *error = saveError;
        return false;
    }
    if (notes) {
        notes->push_back("VM stopped; waiting for user recovery decision");
        notes->push_back("Default fail-safe: host recovery is scheduled for the next restart");
        notes->push_back("Options: reboot to host now, restore host on next restart, or keep GPU assigned to VM");
    }
    scheduleSafetyRecoveryTimer(cfg, notes);
    return true;
}

static bool keepGpuAssignedToVm(AppConfig cfg, QStringList *notes, QString *error) {
    cfg.vmStoppedAwaitingDecision = false;
    cfg.nextBootMode.clear();
    cfg.safetyRecoveryDeadline.clear();
    cfg.mode = "vm";
    cancelSafetyRecovery(notes);
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

# Called by libvirt when the configured VM finishes.  We only record
# a pending decision; the user chooses host-now, host-next-restart,
# or keep-vfio from the GUI/CLI.
case "${OP}:${SUBOP}" in
  stopped:end|release:end)
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
    loadGpuModulesForDriver(driver, notes);
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
                notes_ << "VM handoff failed; attempting best-effort rollback to host ownership";
                QString rollbackError;
                if (!cfg_.audioBdf.isEmpty() && !cfg_.originalAudioDriver.isEmpty()) {
                    (void)recoverDeviceToDriver(cfg_.audioBdf, cfg_.originalAudioDriver, &notes_, &rollbackError);
                }
                if (!cfg_.gpuBdf.isEmpty() && !cfg_.originalGpuDriver.isEmpty()) {
                    (void)recoverDeviceToDriver(cfg_.gpuBdf, cfg_.originalGpuDriver, &notes_, &rollbackError);
                }
                restoreHostConsoleAfterRecovery(&notes_);
                cfg_.mode = "host";
                cfg_.nextBootMode.clear();
                QString saveError;
                (void)saveConfig(cfg_, &saveError);
                (void)writeState("host-rollback", &saveError);
                if (!rollbackError.isEmpty()) notes_ << "Rollback note: " + rollbackError;
                if (!saveError.isEmpty()) notes_ << "Rollback state note: " + saveError;
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
            prepareSingleGpuHostForVfio(preflight_.compatibility.singleGraphicsPath, kind, &notes_);
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
            restoreHostConsoleAfterRecovery(&notes_);
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
                    {"thermalGuardEnabled", cfg.thermalGuardEnabled},
                    {"maxGpuTempC", cfg.maxGpuTempC},
                    {"safetyAutoRecoveryMinutes", cfg.safetyAutoRecoveryMinutes},
                    {"safetyRecoveryDeadline", cfg.safetyRecoveryDeadline},
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

            if (cmd == "laptopCheck") {
                AppConfig cfg = loadConfig();
                const QString bdf = normalizeBdf(req.value("gpuBdf").toString());
                if (!bdf.isEmpty()) cfg.gpuBdf = bdf;
                const auto diag = buildPreflightReport(cfg);
                const auto comp = diag.compatibility;
                sendJson(sock, okReply({
                    {"likelyLaptop", comp.likelyLaptop},
                    {"summary", comp.laptopSafetySummary},
                    {"targetDrmCards", QJsonArray::fromStringList(comp.laptopTargetDrmCards)},
                    {"activeConnectors", QJsonArray::fromStringList(comp.laptopActiveConnectors)},
                    {"internalConnectors", QJsonArray::fromStringList(comp.laptopInternalConnectors)},
                    {"externalConnectors", QJsonArray::fromStringList(comp.laptopExternalConnectors)},
                    {"targetDrivesDisplay", comp.laptopTargetDrivesDisplay},
                    {"targetDrivesInternalPanel", comp.laptopTargetDrivesInternalPanel},
                    {"hybridOffloadCandidate", comp.laptopTargetAppearsSafeHybridOffload},
                    {"diagnostic", QJsonObject::fromVariantMap(preflightToMap(diag))}
                }));
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

            if (cmd == "autoSetupSingleGpu") {
                QString err;
                QJsonObject payload;
                AppConfig cfg = loadConfig();
                if (!autoConfigureSingleGpu(cfg, req, &payload, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                sendJson(sock, okReply(payload));
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
                cfg.thermalGuardEnabled = req.value("thermalGuardEnabled").toBool(cfg.thermalGuardEnabled);
                cfg.maxGpuTempC = req.value("maxGpuTempC").toInt(cfg.maxGpuTempC);
                cfg.safetyAutoRecoveryMinutes = req.value("safetyAutoRecoveryMinutes").toInt(cfg.safetyAutoRecoveryMinutes);
                cfg.safetyRecoveryDeadline = req.value("safetyRecoveryDeadline").toString(cfg.safetyRecoveryDeadline);
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

            if (cmd == "safetyRecoverHostNow") {
                QString err;
                QStringList notes;
                AppConfig cfg = loadConfig();
                if (!cfg.vmStoppedAwaitingDecision) {
                    notes << "Safety recovery timer fired, but no stopped-VM decision is pending; no reboot will be triggered";
                    sendJson(sock, okReply({{"mode", cfg.mode}, {"action", "noop"}, {"notes", QJsonArray::fromStringList(notes)}}));
                    continue;
                }
                if (cfg.safetyAutoRecoveryMinutes <= 0) {
                    notes << "Safety recovery timer fired, but auto recovery is disabled; no reboot will be triggered";
                    sendJson(sock, okReply({{"mode", cfg.mode}, {"action", "noop"}, {"notes", QJsonArray::fromStringList(notes)}}));
                    continue;
                }
                if (!scheduleBootTransition(cfg, "host", &notes, &err)) {
                    sendJson(sock, errorReply(err));
                    continue;
                }
                notes << "Safety recovery timeout reached; rebooting to restore the Linux host GPU driver";
                sendJson(sock, okReply({{"mode", "pending-host"}, {"action", "safety-reboot-now"}, {"notes", QJsonArray::fromStringList(notes)}}));
                QTimer::singleShot(1500, []() { QProcess::startDetached("systemctl", {"reboot"}); });
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
