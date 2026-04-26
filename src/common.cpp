#include "common.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTextStream>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QDateTime>
#include <algorithm>
#include <QVariantList>

namespace gs {

static const QString kAppDir = "/var/lib/gpu-switcher";
static const QString kCfg = kAppDir + "/config.ini";
static const QString kState = kAppDir + "/state.txt";
static const QString kSock = "/run/gpu-switcher.sock";
static const QString kHook = "/etc/libvirt/hooks/qemu";
static const QString kHelper = "/usr/local/bin/gpu-switcher-helperd";
static const QString kCtl = "/usr/local/bin/gpu-switcher-ctl";
static const QString kBackupDir = kAppDir + "/backups";

QString configPath() { return kCfg; }
QString statePath() { return kState; }
QString socketPath() { return kSock; }
QString hookPath() { return kHook; }
QString helperBinaryPath() { return kHelper; }
QString ctlBinaryPath() { return kCtl; }
QString backupDirPath() { return kBackupDir; }

bool ensureRuntimeDirs(QString *error) {
    QDir dir;
    if (!dir.mkpath(kAppDir)) {
        if (error) *error = "Failed to create " + kAppDir;
        return false;
    }
    if (!dir.mkpath(kBackupDir)) {
        if (error) *error = "Failed to create " + kBackupDir;
        return false;
    }
    return true;
}

QString trim(const QString &s) { return s.trimmed(); }

bool readFile(const QString &path, QString *out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    QTextStream ts(&f);
    *out = ts.readAll();
    return true;
}

bool writeFile(const QString &path, const QString &data, QString *error) {
    QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) *error = "Failed to create parent directory for " + path;
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) *error = "Failed to open " + path + " for write";
        return false;
    }
    QTextStream ts(&f);
    ts << data;
    return true;
}

QString shellQuote(const QString &s) {
    QString out = "'";
    for (QChar c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

QString runCommandCapture(const QString &program, const QStringList &args, int timeoutMs, int *exitCode) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted()) {
        if (exitCode) *exitCode = -1;
        return {};
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished();
        if (exitCode) *exitCode = -2;
        return {};
    }
    if (exitCode) *exitCode = p.exitCode();
    return QString::fromUtf8(p.readAllStandardOutput()) + QString::fromUtf8(p.readAllStandardError());
}

QString normalizeBdf(const QString &bdf) {
    const QString s = trim(bdf).toLower();
    if (s.isEmpty()) return {};
    static const QRegularExpression re(R"(^([0-9a-f]{1,4}):([0-9a-f]{1,2}):([0-9a-f]{1,2})\.([0-7])$)");
    const auto m = re.match(s);
    if (!m.hasMatch()) return {};
    const QString dom = m.captured(1).rightJustified(4, '0');
    const QString bus = m.captured(2).rightJustified(2, '0');
    const QString dev = m.captured(3).rightJustified(2, '0');
    const QString func = m.captured(4).left(1);
    return dom + ":" + bus + ":" + dev + "." + func;
}

bool isPciBdf(const QString &bdf) {
    return !normalizeBdf(bdf).isEmpty();
}

QString domainIdForConfig(const AppConfig &cfg) {
    if (!cfg.vmUuid.trimmed().isEmpty()) return cfg.vmUuid.trimmed();
    return cfg.vmName.trimmed();
}

QString normalizeBootTarget(const QString &mode) {
    const QString m = trim(mode).toLower();
    if (m == "vm" || m == "guest" || m == "vfio") return "vm";
    if (m == "host" || m == "pc" || m == "linux") return "host";
    return {};
}

AppConfig loadConfig(QString *error) {
    AppConfig cfg;
    if (!QFileInfo::exists(configPath())) {
        if (error) *error = "config.ini missing";
        return cfg;
    }
    QSettings s(configPath(), QSettings::IniFormat);
    s.beginGroup("app");
    cfg.vmName = s.value("vmName").toString();
    cfg.vmUuid = s.value("vmUuid").toString();
    cfg.gpuBdf = s.value("gpuBdf").toString();
    cfg.audioBdf = s.value("audioBdf").toString();
    cfg.preferredBoot = s.value("preferredBoot", "host").toString();
    cfg.nextBootMode = normalizeBootTarget(s.value("nextBootMode", "").toString());
    cfg.useVendorReset = s.value("useVendorReset", false).toBool();
    cfg.hasFallbackDisplay = s.value("hasFallbackDisplay", false).toBool();
    cfg.allowSingleGpu = s.value("allowSingleGpu", false).toBool();
    cfg.autoStartVmOnBoot = s.value("autoStartVmOnBoot", true).toBool();
    cfg.thermalGuardEnabled = s.value("thermalGuardEnabled", true).toBool();
    cfg.maxGpuTempC = s.value("maxGpuTempC", 85).toInt();
    if (cfg.maxGpuTempC < 60 || cfg.maxGpuTempC > 105) cfg.maxGpuTempC = 85;
    cfg.safetyAutoRecoveryMinutes = s.value("safetyAutoRecoveryMinutes", 10).toInt();
    if (cfg.safetyAutoRecoveryMinutes < 0 || cfg.safetyAutoRecoveryMinutes > 240) cfg.safetyAutoRecoveryMinutes = 10;
    cfg.safetyRecoveryDeadline = s.value("safetyRecoveryDeadline").toString();
    cfg.vmStoppedAwaitingDecision = s.value("vmStoppedAwaitingDecision", false).toBool();
    cfg.lastStoppedVm = s.value("lastStoppedVm").toString();
    cfg.lastStoppedAt = s.value("lastStoppedAt").toString();
    cfg.mode = s.value("mode", "host").toString();
    cfg.originalGpuDriver = s.value("originalGpuDriver").toString();
    cfg.originalAudioDriver = s.value("originalAudioDriver").toString();
    s.endGroup();
    Q_UNUSED(error);
    return cfg;
}

bool saveConfig(const AppConfig &cfg, QString *error) {
    if (!ensureRuntimeDirs(error)) return false;
    if (!isPciBdf(cfg.gpuBdf)) {
        if (error) *error = "GPU BDF is invalid";
        return false;
    }
    if (!cfg.audioBdf.trimmed().isEmpty() && !isPciBdf(cfg.audioBdf)) {
        if (error) *error = "Audio BDF is invalid";
        return false;
    }
    QSettings s(configPath(), QSettings::IniFormat);
    s.beginGroup("app");
    s.setValue("vmName", cfg.vmName);
    s.setValue("vmUuid", cfg.vmUuid);
    s.setValue("gpuBdf", normalizeBdf(cfg.gpuBdf));
    s.setValue("audioBdf", normalizeBdf(cfg.audioBdf));
    s.setValue("preferredBoot", cfg.preferredBoot);
    s.setValue("nextBootMode", normalizeBootTarget(cfg.nextBootMode));
    s.setValue("useVendorReset", cfg.useVendorReset);
    s.setValue("hasFallbackDisplay", cfg.hasFallbackDisplay);
    s.setValue("allowSingleGpu", cfg.allowSingleGpu);
    s.setValue("autoStartVmOnBoot", cfg.autoStartVmOnBoot);
    s.setValue("thermalGuardEnabled", cfg.thermalGuardEnabled);
    s.setValue("maxGpuTempC", std::clamp(cfg.maxGpuTempC, 60, 105));
    s.setValue("safetyAutoRecoveryMinutes", std::clamp(cfg.safetyAutoRecoveryMinutes, 0, 240));
    s.setValue("safetyRecoveryDeadline", cfg.safetyRecoveryDeadline);
    s.setValue("vmStoppedAwaitingDecision", cfg.vmStoppedAwaitingDecision);
    s.setValue("lastStoppedVm", cfg.lastStoppedVm);
    s.setValue("lastStoppedAt", cfg.lastStoppedAt);
    s.setValue("mode", cfg.mode);
    s.setValue("originalGpuDriver", cfg.originalGpuDriver);
    s.setValue("originalAudioDriver", cfg.originalAudioDriver);
    s.endGroup();
    s.sync();
    if (s.status() != QSettings::NoError) {
        if (error) *error = "Failed to write config.ini";
        return false;
    }
    return true;
}

QString readState(QString *error) {
    QString data;
    if (!readFile(statePath(), &data)) {
        if (error) *error = "state file missing";
        return {};
    }
    return trim(data);
}

bool writeState(const QString &state, QString *error) {
    if (!ensureRuntimeDirs(error)) return false;
    return writeFile(statePath(), state + "\n", error);
}

static QString sysfsPath(const QString &bdf) {
    return "/sys/bus/pci/devices/" + normalizeBdf(bdf);
}

static QString readText(const QString &path) {
    QString s;
    if (!readFile(path, &s)) return {};
    return trim(s);
}

static QString resolveDriverName(const QString &bdf) {
    QFileInfo link(sysfsPath(bdf) + "/driver");
    if (!link.exists()) return {};
    const QString target = QFile::symLinkTarget(link.absoluteFilePath());
    if (target.isEmpty()) return {};
    return QFileInfo(target).fileName();
}

static QString groupNameForDevice(const QString &bdf) {
    QFileInfo groupLink(sysfsPath(bdf) + "/iommu_group");
    if (!groupLink.exists()) return {};
    const QString target = QFile::symLinkTarget(groupLink.absoluteFilePath());
    if (target.isEmpty()) return {};
    return QFileInfo(target).fileName();
}

bool iommuEnabled(QString *detail) {
    const bool groupsExist = QFileInfo::exists("/sys/kernel/iommu_groups") && !QDir("/sys/kernel/iommu_groups").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    if (detail) *detail = groupsExist ? "iommu_groups present" : "iommu_groups missing";
    return groupsExist;
}

QStringList devicesInIommuGroup(QString groupName, QString *detail) {
    QStringList devs;
    if (groupName.isEmpty()) return devs;
    QDir dir("/sys/kernel/iommu_groups/" + groupName + "/devices");
    const auto entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &e : entries) devs.push_back(e);
    if (detail) *detail = QString::number(devs.size()) + " device(s) in group " + groupName;
    return devs;
}

QStringList allGraphicsControllers(QString *detail) {
    QStringList out;
    QDir dir("/sys/bus/pci/devices");
    const auto entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &bdf : entries) {
        const QString cls = readText(sysfsPath(bdf) + "/class");
        if (cls.startsWith("0x03")) {
            out.push_back(bdf);
        }
    }
    if (detail) *detail = QString::number(out.size()) + " graphics/controller device(s) found";
    return out;
}

bool systemHasFallbackDisplay(QString *detail) {
    QString info;
    const auto gfx = allGraphicsControllers(&info);
    const bool hasFallback = gfx.size() >= 2;
    if (detail) *detail = info + (hasFallback ? " (fallback likely present)" : " (single graphics controller likely)");
    return hasFallback;
}

bool singleGPUOnly(QString gpuBdf, QString *detail) {
    QString info;
    const auto gfx = allGraphicsControllers(&info);
    const QString gpu = normalizeBdf(gpuBdf);
    const bool single = (gfx.size() <= 1) || (gfx.size() == 1 && normalizeBdf(gfx.first()) == gpu);
    if (detail) *detail = info + (single ? " (single-GPU path)" : " (multi-GPU path)");
    return single;
}

bool iommuGroupIsSafe(const QString &gpuBdf, QStringList *reasons) {
    const QString normalized = normalizeBdf(gpuBdf);
    const QString group = groupNameForDevice(normalized);
    if (group.isEmpty()) {
        if (reasons) reasons->push_back("GPU has no visible IOMMU group");
        return false;
    }

    const QStringList members = devicesInIommuGroup(group, nullptr);
    if (members.isEmpty()) {
        if (reasons) reasons->push_back("IOMMU group is empty or unreadable");
        return false;
    }

    bool safe = true;
    for (const auto &dev : members) {
        if (normalizeBdf(dev) == normalized) continue;
        const QString cls = readText(sysfsPath(dev) + "/class");
        const bool looksLikeGpuOrAudio = cls.startsWith("0x03") || cls.startsWith("0x04");
        if (!looksLikeGpuOrAudio) {
            safe = false;
            if (reasons) reasons->push_back("Group " + group + " also contains non-GPU device " + dev + " class=" + cls);
        }
    }
    if (reasons && safe) reasons->push_back("IOMMU group " + group + " contains only GPU-related devices");
    return safe;
}


bool readGpuTemperatureC(const QString &bdf, int *temperatureC, QString *sensorPath, QString *detail) {
    const QString normalized = normalizeBdf(bdf);
    const QString base = sysfsPath(normalized) + "/hwmon";
    QDir hwmonDir(base);
    if (!hwmonDir.exists()) {
        if (detail) *detail = "No hwmon directory for " + normalized;
        return false;
    }

    int bestMilliC = -1;
    QString bestPath;
    const auto hwmons = hwmonDir.entryList(QStringList{"hwmon*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &hwmon : hwmons) {
        QDir d(hwmonDir.absoluteFilePath(hwmon));
        const auto sensors = d.entryList(QStringList{"temp*_input"}, QDir::Files);
        for (const auto &sensor : sensors) {
            bool ok = false;
            const int milliC = readText(d.absoluteFilePath(sensor)).toInt(&ok);
            if (!ok || milliC <= 0) continue;
            if (milliC > bestMilliC) {
                bestMilliC = milliC;
                bestPath = d.absoluteFilePath(sensor);
            }
        }
    }

    if (bestMilliC < 0) {
        if (detail) *detail = "No readable temp*_input sensor under " + base;
        return false;
    }

    const int c = bestMilliC / 1000;
    if (temperatureC) *temperatureC = c;
    if (sensorPath) *sensorPath = bestPath;
    if (detail) *detail = QString("GPU temperature %1 C from %2").arg(c).arg(bestPath);
    return true;
}

DeviceInfo inspectDevice(const QString &bdf) {
    const QString normalized = normalizeBdf(bdf);
    DeviceInfo d;
    d.bdf = normalized;
    const QString base = sysfsPath(normalized);
    d.exists = QFileInfo::exists(base);
    if (!d.exists) return d;
    d.vendorId = readText(base + "/vendor");
    d.deviceId = readText(base + "/device");
    d.classCode = readText(base + "/class");
    d.driverName = resolveDriverName(normalized);
    d.hasResetNode = QFileInfo::exists(base + "/reset");
    d.resetMethod = readText(base + "/reset_method");
    d.iommuGroup = groupNameForDevice(normalized);
    int tempC = -1;
    QString tempPath;
    d.temperatureReadable = readGpuTemperatureC(normalized, &tempC, &tempPath, nullptr);
    d.temperatureC = tempC;
    d.temperaturePath = tempPath;
    return d;
}

QStringList supportedResetMethods(const QString &bdf) {
    const QString raw = readText(sysfsPath(bdf) + "/reset_method");
    QStringList methods;
    for (const auto &m : raw.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
        methods << m.trimmed();
    }
    return methods;
}

bool deviceVendorIsAmd(const DeviceInfo &d) {
    return d.vendorId.contains("0x1002", Qt::CaseInsensitive) || d.vendorId.contains("1002", Qt::CaseInsensitive);
}

bool deviceVendorIsNvidia(const DeviceInfo &d) {
    return d.vendorId.contains("0x10de", Qt::CaseInsensitive) || d.vendorId.contains("10de", Qt::CaseInsensitive);
}

VendorKind vendorKind(const DeviceInfo &d) {
    if (deviceVendorIsAmd(d)) return VendorKind::AMD;
    if (deviceVendorIsNvidia(d)) return VendorKind::NVIDIA;
    if (d.vendorId.contains("0x8086", Qt::CaseInsensitive) || d.vendorId.contains("8086", Qt::CaseInsensitive)) return VendorKind::Intel;
    if (!d.vendorId.trimmed().isEmpty()) return VendorKind::Other;
    return VendorKind::Unknown;
}

QString vendorKindName(VendorKind kind) {
    switch (kind) {
    case VendorKind::AMD: return "AMD";
    case VendorKind::NVIDIA: return "NVIDIA";
    case VendorKind::Intel: return "Intel";
    case VendorKind::Other: return "Other";
    case VendorKind::Unknown:
    default: return "Unknown";
    }
}

static bool serviceActive(const QString &service, QString *detail = nullptr) {
    int exitCode = 0;
    (void)runCommandCapture("systemctl", {"is-active", "--quiet", service}, 4000, &exitCode);
    const bool ok = (exitCode == 0);
    if (detail) *detail = service + (ok ? " active" : " inactive or unavailable");
    return ok;
}

bool hookInstalled(QString *detail) {
    QFileInfo info(hookPath());
    const bool ok = info.exists() && info.isFile() && info.isExecutable();
    if (detail) {
        *detail = ok ? (hookPath() + " present and executable") : (hookPath() + " missing or not executable");
    }
    return ok;
}

static QString extractXmlText(const QString &xml, const QString &name) {
    QXmlStreamReader xr(xml);
    while (!xr.atEnd()) {
        xr.readNext();
        if (xr.isStartElement() && xr.name() == name) return xr.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
    }
    return {};
}

static bool parseHostdevs(const QString &xml, QStringList *devices) {
    QXmlStreamReader xr(xml);
    bool inHostdev = false;
    bool hostdevIsPci = false;
    bool hostdevManaged = false;
    QString currentDomain, currentBus, currentSlot, currentFunction;
    bool inSource = false;

    while (!xr.atEnd()) {
        xr.readNext();
        if (xr.isStartElement()) {
            const auto name = xr.name().toString();
            if (name == "hostdev") {
                inHostdev = true;
                hostdevIsPci = (xr.attributes().value("type") == "pci");
                hostdevManaged = (xr.attributes().value("managed") == "yes");
                currentDomain.clear(); currentBus.clear(); currentSlot.clear(); currentFunction.clear();
            } else if (inHostdev && name == "source") {
                inSource = true;
            } else if (inHostdev && inSource && name == "address") {
                currentDomain = xr.attributes().value("domain").toString();
                currentBus = xr.attributes().value("bus").toString();
                currentSlot = xr.attributes().value("slot").toString();
                currentFunction = xr.attributes().value("function").toString();
            }
        } else if (xr.isEndElement()) {
            if (xr.name() == "source") inSource = false;
            if (xr.name() == "hostdev") {
                Q_UNUSED(hostdevManaged);
                if (hostdevIsPci && !currentBus.isEmpty() && !currentSlot.isEmpty() && !currentFunction.isEmpty()) {
                    auto hex2 = [](QString v) {
                        v = v.trimmed().toLower();
                        if (v.startsWith("0x")) v.remove(0, 2);
                        return v.rightJustified(v.size() <= 2 ? 2 : v.size(), '0');
                    };
                    const QString dom = currentDomain.isEmpty() ? "0000" : hex2(currentDomain);
                    const QString bus = hex2(currentBus);
                    const QString slot = hex2(currentSlot);
                    const QString fn = hex2(currentFunction);
                    devices->push_back(dom + ":" + bus + ":" + slot + "." + fn.left(1));
                }
                inHostdev = false;
            }
        }
    }
    return !devices->isEmpty();
}

bool vmXmlMatchesConfig(const AppConfig &cfg, QStringList *issues, QString *rawXml) {
    const QString domain = domainIdForConfig(cfg);
    if (domain.isEmpty()) {
        if (issues) issues->push_back("VM name/UUID is not configured");
        return false;
    }

    QString xml;
    int exitCode = 0;
    xml = runCommandCapture("virsh", {"-c", "qemu:///system", "dumpxml", "--inactive", domain}, 10000, &exitCode);
    if (exitCode != 0 || xml.trimmed().isEmpty()) {
        if (issues) issues->push_back("Unable to read libvirt XML for domain " + domain);
        return false;
    }
    if (rawXml) *rawXml = xml;

    QStringList hostdevs;
    parseHostdevs(xml, &hostdevs);
    const QString gpu = normalizeBdf(cfg.gpuBdf);
    const QString audio = normalizeBdf(cfg.audioBdf);

    const auto containsDev = [&hostdevs](const QString &bdf) {
        return hostdevs.contains(normalizeBdf(bdf), Qt::CaseInsensitive);
    };

    bool ok = true;
    if (!containsDev(gpu)) {
        ok = false;
        if (issues) issues->push_back("Libvirt XML does not contain the configured GPU BDF " + gpu);
    }
    if (!audio.isEmpty() && !containsDev(audio)) {
        ok = false;
        if (issues) issues->push_back("Libvirt XML does not contain the configured audio function " + audio);
    }
    if (issues && ok) issues->push_back("Libvirt XML matches the configured passthrough devices");
    return ok;
}

bool backupVmXml(const AppConfig &cfg, QString *backupPath, QString *error) {
    const QString domain = domainIdForConfig(cfg);
    if (domain.isEmpty()) {
        if (error) *error = "VM name/UUID is not configured";
        return false;
    }
    QString xml;
    QStringList issues;
    if (!vmXmlMatchesConfig(cfg, &issues, &xml)) {
        if (error) *error = issues.join("; ");
        return false;
    }
    const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
    QString safeDomain = domain;
    safeDomain.replace('/', '_');
    const QString path = backupDirPath() + "/" + safeDomain + "-" + stamp + ".xml";
    if (!writeFile(path, xml, error)) return false;
    if (backupPath) *backupPath = path;
    return true;
}

bool canUseVendorReset(const DeviceInfo &gpu, QString *detail) {
    if (!deviceVendorIsAmd(gpu)) {
        if (detail) *detail = "vendor-reset is AMD-only; skipped";
        return false;
    }
    const bool loaded = QFileInfo::exists("/sys/module/vendor_reset") || QFileInfo::exists("/sys/module/vendor-reset") || QFileInfo::exists("/sys/module/vendor_reset/parameters");
    if (detail) *detail = loaded ? "vendor-reset module is available" : "vendor-reset module not loaded";
    return loaded;
}

bool pciReset(const QString &bdf, QString *error) {
    const QString path = sysfsPath(bdf) + "/reset";
    QFile f(path);
    if (!f.exists()) {
        if (error) *error = "No per-device reset node for " + normalizeBdf(bdf);
        return false;
    }
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Failed to open reset node for " + normalizeBdf(bdf);
        return false;
    }
    if (f.write("1") < 0) {
        if (error) *error = "Failed to trigger reset for " + normalizeBdf(bdf);
        return false;
    }
    return true;
}

bool bestEffortReset(const QString &bdf, const AppConfig &cfg, QStringList *notes, QString *error) {
    const DeviceInfo dev = inspectDevice(bdf);
    if (!dev.exists) {
        if (error) *error = "Device not found: " + normalizeBdf(bdf);
        return false;
    }

    if (notes) {
        notes->push_back("Vendor: " + (deviceVendorIsAmd(dev) ? "AMD" : deviceVendorIsNvidia(dev) ? "NVIDIA" : dev.vendorId));
        notes->push_back("Reset methods: " + (dev.resetMethod.isEmpty() ? "none exposed" : dev.resetMethod));
    }

    if (dev.hasResetNode) {
        if (pciReset(dev.bdf, nullptr)) {
            if (notes) notes->push_back("Reset via /reset succeeded");
            return true;
        }
        if (notes) notes->push_back("Reset via /reset failed");
    }

    if (deviceVendorIsAmd(dev) && cfg.useVendorReset) {
        QProcess p;
        p.start("modprobe", {"vendor-reset"});
        p.waitForFinished(7000);
        if (notes) notes->push_back("modprobe vendor-reset exit=" + QString::number(p.exitCode()));
        if (p.exitStatus() == QProcess::NormalExit) {
            if (pciReset(dev.bdf, nullptr)) {
                if (notes) notes->push_back("Reset after vendor-reset load succeeded");
                return true;
            }
            if (notes) notes->push_back("Reset after vendor-reset load still failed");
        }
    }

    if (notes) notes->push_back("No reliable reset path available; reboot fallback required");
    if (error) *error = "GPU reset did not complete cleanly for " + normalizeBdf(bdf);
    return false;
}

bool unbindDriver(const QString &bdf, QString *error) {
    const QString normalized = normalizeBdf(bdf);
    const QString drv = resolveDriverName(normalized);
    if (drv.isEmpty()) return true;
    QFile f("/sys/bus/pci/drivers/" + drv + "/unbind");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Failed to open unbind for " + normalized;
        return false;
    }
    if (f.write(normalized.toUtf8()) < 0) {
        if (error) *error = "Failed to unbind " + normalized + " from " + drv;
        return false;
    }
    return true;
}

bool bindToDriver(const QString &bdf, const QString &driver, QString *error) {
    const QString normalized = normalizeBdf(bdf);
    QFile f("/sys/bus/pci/drivers/" + driver + "/bind");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Failed to open bind for " + driver;
        return false;
    }
    if (f.write(normalized.toUtf8()) < 0) {
        if (error) *error = "Failed to bind " + normalized + " to " + driver;
        return false;
    }
    return true;
}

bool setDriverOverride(const QString &bdf, const QString &driver, QString *error) {
    const QString normalized = normalizeBdf(bdf);
    QFile f(sysfsPath(normalized) + "/driver_override");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Failed to open driver_override for " + normalized;
        return false;
    }
    if (f.write(driver.toUtf8()) < 0) {
        if (error) *error = "Failed to write driver_override for " + normalized;
        return false;
    }
    return true;
}

bool clearDriverOverride(const QString &bdf, QString *error) {
    return setDriverOverride(bdf, "", error);
}

bool removeFromBus(const QString &bdf, QString *error) {
    const QString normalized = normalizeBdf(bdf);
    QFile f(sysfsPath(normalized) + "/remove");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Failed to open remove for " + normalized;
        return false;
    }
    if (f.write("1") < 0) {
        if (error) *error = "Failed to remove " + normalized + " from PCI bus";
        return false;
    }
    return true;
}

bool rescanBus(QString *error) {
    QFile f("/sys/bus/pci/rescan");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Failed to open PCI rescan";
        return false;
    }
    if (f.write("1") < 0) {
        if (error) *error = "Failed to rescan PCI bus";
        return false;
    }
    return true;
}

QVariantMap deviceInfoToMap(const DeviceInfo &d) {
    QVariantMap m;
    m["bdf"] = d.bdf;
    m["vendorId"] = d.vendorId;
    m["deviceId"] = d.deviceId;
    m["classCode"] = d.classCode;
    m["driverName"] = d.driverName;
    m["resetMethod"] = d.resetMethod;
    m["iommuGroup"] = d.iommuGroup;
    m["hasResetNode"] = d.hasResetNode;
    m["exists"] = d.exists;
    m["vendor"] = deviceVendorIsAmd(d) ? "AMD" : deviceVendorIsNvidia(d) ? "NVIDIA" : "Other";
    m["temperatureReadable"] = d.temperatureReadable;
    m["temperatureC"] = d.temperatureC;
    m["temperaturePath"] = d.temperaturePath;
    return m;
}

QVariantMap reportToMap(const CompatibilityReport &r) {
    QVariantMap m;
    m["iommuEnabled"] = r.iommuEnabled;
    m["gpuExists"] = r.gpuExists;
    m["groupSafe"] = r.groupSafe;
    m["hasFallbackDisplay"] = r.hasFallbackDisplay;
    m["singleGraphicsPath"] = r.singleGraphicsPath;
    m["resetAvailable"] = r.resetAvailable;
    m["vmXmlMatches"] = r.vmXmlMatches;
    m["hookInstalled"] = r.hookInstalled;
    m["vendorResetAvailable"] = r.vendorResetAvailable;
    m["secureBootEnabled"] = r.secureBootEnabled;
    m["displayManagerActive"] = r.displayManagerActive;
    m["sshdActive"] = r.sshdActive;
    m["nvidiaHiddenState"] = r.nvidiaHiddenState;
    m["gpuHasAudioCompanion"] = r.gpuHasAudioCompanion;
    m["gpuTemperatureReadable"] = r.gpuTemperatureReadable;
    m["gpuTemperatureC"] = r.gpuTemperatureC;
    m["gpuTemperaturePath"] = r.gpuTemperaturePath;
    m["thermalGuardEnabled"] = r.thermalGuardEnabled;
    m["maxGpuTempC"] = r.maxGpuTempC;
    m["safetyAutoRecoveryMinutes"] = r.safetyAutoRecoveryMinutes;
    m["safetyRecoveryDeadline"] = r.safetyRecoveryDeadline;
    m["gpuModel"] = r.gpuModel;
    m["kernelCmdline"] = r.kernelCmdline;
    m["vendorName"] = r.vendorName;
    m["domainId"] = r.domainId;
    m["reasons"] = r.reasons;
    m["warnings"] = r.warnings;
    return m;
}

static bool vmXmlHasHiddenKvmState(const QString &xml);

CompatibilityReport buildCompatibilityReport(const AppConfig &cfg) {
    CompatibilityReport r;
    QString detail;
    r.domainId = domainIdForConfig(cfg);
    r.iommuEnabled = iommuEnabled(&detail);
    if (!r.iommuEnabled) r.reasons << "IOMMU not detected in /sys/kernel/iommu_groups";

    const QString gpuBdf = normalizeBdf(cfg.gpuBdf);
    r.gpuExists = !gpuBdf.isEmpty() && QFileInfo::exists(sysfsPath(gpuBdf));
    if (!r.gpuExists) r.reasons << "GPU device not found: " + gpuBdf;

    const DeviceInfo gpu = inspectDevice(gpuBdf);
    r.vendorName = deviceVendorIsAmd(gpu) ? "AMD" : deviceVendorIsNvidia(gpu) ? "NVIDIA" : (!gpu.vendorId.isEmpty() ? gpu.vendorId : "Unknown");
    r.gpuModel = gpu.deviceId;
    r.gpuTemperatureReadable = gpu.temperatureReadable;
    r.gpuTemperatureC = gpu.temperatureC;
    r.gpuTemperaturePath = gpu.temperaturePath;
    r.thermalGuardEnabled = cfg.thermalGuardEnabled;
    r.maxGpuTempC = cfg.maxGpuTempC;
    r.safetyAutoRecoveryMinutes = cfg.safetyAutoRecoveryMinutes;
    r.safetyRecoveryDeadline = cfg.safetyRecoveryDeadline;
    r.kernelCmdline = kernelCmdline(&detail);
    r.resetAvailable = gpu.hasResetNode;
    r.vendorResetAvailable = canUseVendorReset(gpu, &detail);
    if (gpu.exists && gpu.hasResetNode) {
        r.warnings << "Kernel reset node exists for the GPU; recovery should still keep a reboot fallback.";
    }
    if (gpu.exists && !gpu.hasResetNode) {
        r.warnings << "This GPU does not expose /reset; vendor-specific or reboot recovery may be required.";
    }

    r.groupSafe = cfg.gpuBdf.isEmpty() ? false : iommuGroupIsSafe(cfg.gpuBdf, &r.reasons);
    r.hasFallbackDisplay = cfg.hasFallbackDisplay || systemHasFallbackDisplay(&detail);
    r.displayManagerActive = displayManagerActive(&detail);
    r.sshdActive = sshdActive(&detail);
    r.secureBootEnabled = secureBootEnabled(&detail);
    r.singleGraphicsPath = cfg.gpuBdf.isEmpty() ? false : singleGPUOnly(cfg.gpuBdf, &detail);

    if (r.singleGraphicsPath && !cfg.hasFallbackDisplay && !cfg.allowSingleGpu) {
        r.warnings << "Single-GPU systems need a second display path, SSH access, or explicit acknowledgement.";
    }
    if (!cfg.hasFallbackDisplay && !r.sshdActive) {
        r.warnings << "Fallback display / SSH recovery is not confirmed.";
    }

    r.hookInstalled = hookInstalled(&detail);
    if (!r.hookInstalled) {
        r.warnings << "Libvirt hook is missing or not executable.";
    }

    r.gpuHasAudioCompanion = !companionAudioFunctions(cfg.gpuBdf, nullptr).isEmpty();
    if (!cfg.vmName.trimmed().isEmpty() || !cfg.vmUuid.trimmed().isEmpty()) {
        QString xml;
        r.vmXmlMatches = vmXmlMatchesConfig(cfg, &r.reasons, &xml);
        if (r.vmXmlMatches && deviceVendorIsNvidia(gpu)) {
            r.nvidiaHiddenState = vmXmlHasHiddenKvmState(xml);
            if (!r.nvidiaHiddenState) {
                r.warnings << "NVIDIA XML does not hide KVM; older drivers may require hidden state.";
            }
        }
        if (!r.vmXmlMatches) r.warnings << "Libvirt XML does not yet match the configured passthrough devices.";
    }

    return r;
}



QVariantMap preflightToMap(const PreflightReport &r) {
    QVariantMap m;
    m["canEnterVm"] = r.canEnterVm;
    m["vendor"] = vendorKindName(r.vendor);
    m["vendorName"] = r.vendorName;
    m["config"] = QVariantMap{
        {"vmName", r.config.vmName},
        {"vmUuid", r.config.vmUuid},
        {"gpuBdf", r.config.gpuBdf},
        {"audioBdf", r.config.audioBdf},
        {"preferredBoot", r.config.preferredBoot},
        {"useVendorReset", r.config.useVendorReset},
        {"hasFallbackDisplay", r.config.hasFallbackDisplay},
        {"allowSingleGpu", r.config.allowSingleGpu},
        {"autoStartVmOnBoot", r.config.autoStartVmOnBoot},
        {"thermalGuardEnabled", r.config.thermalGuardEnabled},
        {"maxGpuTempC", r.config.maxGpuTempC},
        {"safetyAutoRecoveryMinutes", r.config.safetyAutoRecoveryMinutes},
        {"safetyRecoveryDeadline", r.config.safetyRecoveryDeadline},
        {"vmStoppedAwaitingDecision", r.config.vmStoppedAwaitingDecision},
        {"lastStoppedVm", r.config.lastStoppedVm},
        {"lastStoppedAt", r.config.lastStoppedAt},
        {"mode", r.config.mode},
        {"originalGpuDriver", r.config.originalGpuDriver},
        {"originalAudioDriver", r.config.originalAudioDriver},
    };
    m["compatibility"] = reportToMap(r.compatibility);
    m["stages"] = r.stages;
    m["blockers"] = r.blockers;
    m["warnings"] = r.warnings;
    m["notes"] = r.notes;
    QVariantList findings;
    for (const auto &f : r.findings) {
        findings.push_back(QVariantMap{
            {"severity", f.severity == DiagnosticSeverity::Blocker ? "blocker" : f.severity == DiagnosticSeverity::Warning ? "warning" : "info"},
            {"code", f.code},
            {"title", f.title},
            {"detail", f.detail},
            {"remediation", f.remediation},
        });
    }
    m["findings"] = findings;
    return m;
}

PreflightReport buildPreflightReport(const AppConfig &cfg) {
    PreflightReport r;
    r.config = cfg;
    r.compatibility = buildCompatibilityReport(cfg);
    r.vendor = vendorKind(inspectDevice(normalizeBdf(cfg.gpuBdf)));
    r.vendorName = vendorKindName(r.vendor);
    r.canEnterVm = false;

    const auto addFinding = [&r](DiagnosticSeverity sev, const QString &code, const QString &title, const QString &detail, const QString &remediation) {
        r.findings.push_back(PreflightFinding{sev, code, title, detail, remediation});
        if (sev == DiagnosticSeverity::Blocker) r.blockers.push_back(title + ": " + detail);
        else if (sev == DiagnosticSeverity::Warning) r.warnings.push_back(title + ": " + detail);
        else r.notes.push_back(title + ": " + detail);
    };

    r.stages = {"validate-config", "detect-vendor", "check-iommu", "check-group", "check-reset", "check-thermal-guard", "check-libvirt", "check-host-recovery"};

    if (!isPciBdf(cfg.gpuBdf)) {
        addFinding(DiagnosticSeverity::Blocker, "invalid-gpu-bdf", "GPU PCI address is invalid", cfg.gpuBdf, "Enter a PCI address in the form 0000:01:00.0");
    }
    if (!cfg.audioBdf.trimmed().isEmpty() && !isPciBdf(cfg.audioBdf)) {
        addFinding(DiagnosticSeverity::Blocker, "invalid-audio-bdf", "Audio PCI address is invalid", cfg.audioBdf, "Enter the companion HDMI/DP audio PCI address or leave it blank");
    }
    if (!r.compatibility.iommuEnabled) {
        addFinding(DiagnosticSeverity::Blocker, "iommu-missing", "IOMMU is not available", "The kernel does not expose usable iommu_groups", "Enable VT-d/AMD-Vi in firmware and boot with the required kernel parameters");
    }
    if (!r.compatibility.gpuExists) {
        addFinding(DiagnosticSeverity::Blocker, "gpu-missing", "GPU device is not present", "The selected PCI device is not visible under /sys/bus/pci/devices", "Confirm the PCI address and that the device is not disabled by firmware");
    }
    if (!r.compatibility.groupSafe) {
        addFinding(DiagnosticSeverity::Blocker, "unsafe-iommu-group", "IOMMU group is not safe", "The GPU shares a group with unrelated hardware", "Move the card to a slot with proper ACS separation or use a board/firmware with safer isolation");
    }
    if (r.compatibility.singleGraphicsPath && !cfg.hasFallbackDisplay && !cfg.allowSingleGpu) {
        addFinding(DiagnosticSeverity::Blocker, "single-gpu-path", "Single-GPU passthrough is not acknowledged", "The host appears to have only one graphics path", "Provide an iGPU, second GPU, or SSH/headless recovery and explicitly enable the acknowledgement");
    } else if (r.compatibility.singleGraphicsPath) {
        addFinding(DiagnosticSeverity::Warning, "single-gpu-fragile", "Single-GPU mode is fragile", "The host may lose the display during VM ownership", "Keep SSH or another recovery path active and expect reboot fallback");
    }
    if (!r.compatibility.vmXmlMatches) {
        addFinding(DiagnosticSeverity::Blocker, "xml-mismatch", "Libvirt XML does not match the app configuration", "The configured hostdev PCI functions are not found in the inactive domain XML", "Back up and update the domain XML or reconfigure the GPU/audio PCI addresses");
    }
    if (!r.compatibility.hookInstalled) {
        addFinding(DiagnosticSeverity::Blocker, "hook-missing", "Libvirt QEMU hook is not installed or executable", "The lifecycle hook path is missing or not executable", "Install the hook to /etc/libvirt/hooks/qemu and make it executable");
    }
    if (!r.compatibility.resetAvailable) {
        addFinding(DiagnosticSeverity::Warning, "no-clean-reset", "GPU does not expose a clean reset node", "The kernel did not expose /reset for this device", "Keep reboot fallback available; AMD systems may benefit from vendor-reset");
    }
    if (cfg.thermalGuardEnabled) {
        if (r.compatibility.gpuTemperatureReadable) {
            const QString tempDetail = QString("Current GPU temperature is %1 C; configured limit is %2 C").arg(r.compatibility.gpuTemperatureC).arg(cfg.maxGpuTempC);
            if (r.compatibility.gpuTemperatureC >= cfg.maxGpuTempC) {
                addFinding(DiagnosticSeverity::Blocker, "gpu-temperature-too-high", "GPU temperature guard blocked VM mode", tempDetail, "Let the GPU cool down, check case airflow/fans, then run preflight again");
            } else if (r.compatibility.gpuTemperatureC >= cfg.maxGpuTempC - 10) {
                addFinding(DiagnosticSeverity::Warning, "gpu-temperature-near-limit", "GPU temperature is close to the guard limit", tempDetail, "Improve airflow or raise the limit only if you know your GPU's safe operating range");
            } else {
                addFinding(DiagnosticSeverity::Info, "gpu-temperature-ok", "GPU temperature guard passed", tempDetail, "No action needed");
            }
        } else {
            addFinding(DiagnosticSeverity::Warning, "gpu-temperature-unreadable", "GPU temperature sensor is not readable from Linux", "No hwmon temp*_input sensor was found for the configured GPU", "Keep the default post-VM auto recovery enabled and verify fan behavior in the guest OS");
        }
    } else {
        addFinding(DiagnosticSeverity::Warning, "thermal-guard-disabled", "GPU temperature guard is disabled", "The app will not block VM mode based on Linux sensor temperature", "Enable the thermal guard unless you have an external monitoring plan");
    }
    if (cfg.safetyAutoRecoveryMinutes == 0) {
        addFinding(DiagnosticSeverity::Warning, "auto-recovery-disabled", "VM-stop safety recovery timer is disabled", "The GPU may remain bound to VFIO after the VM stops", "Use a positive auto-recovery value unless you intentionally manage recovery manually");
    }
    if (r.vendor == VendorKind::AMD && !r.compatibility.vendorResetAvailable && cfg.useVendorReset) {
        addFinding(DiagnosticSeverity::Warning, "vendor-reset-missing", "AMD vendor-reset is requested but not available", "The vendor-reset module is not loaded or not present", "Install and load vendor-reset if your hardware requires it");
    }
    if (r.vendor == VendorKind::NVIDIA) {
        addFinding(DiagnosticSeverity::Info, "nvidia-detected", "NVIDIA GPU detected", "Use host passthrough mode, hidden KVM flags, and reboot fallback for recovery", "Keep the NVIDIA host driver unloaded while the GPU is assigned to VFIO");
    } else if (r.vendor == VendorKind::AMD) {
        addFinding(DiagnosticSeverity::Info, "amd-detected", "AMD GPU detected", "AMD reset behavior is hardware-specific", "Use vendor-reset when needed and retain reboot recovery");
    }

    QString detail;
    const bool fallback = cfg.hasFallbackDisplay || systemHasFallbackDisplay(&detail) || serviceActive("sshd", &detail);
    if (!fallback) {
        addFinding(DiagnosticSeverity::Warning, "no-fallback-display", "No fallback display or SSH recovery was confirmed", detail, "Provide SSH access, an iGPU, or a second GPU for safe recovery");
    }

    r.canEnterVm = r.findings.isEmpty() || std::none_of(r.findings.begin(), r.findings.end(), [](const PreflightFinding &f) { return f.severity == DiagnosticSeverity::Blocker; });
    return r;
}

QString kernelCmdline(QString *detail) {
    QString cmdline;
    if (!readFile("/proc/cmdline", &cmdline)) {
        if (detail) *detail = "Unable to read /proc/cmdline";
        return {};
    }
    cmdline = trim(cmdline);
    if (detail) *detail = cmdline;
    return cmdline;
}

static bool secureBootFromMokutil(QString *detail) {
    int exitCode = 0;
    const QString out = runCommandCapture("mokutil", {"--sb-state"}, 5000, &exitCode);
    const QString lower = out.toLower();
    if (exitCode == 0 && lower.contains("enabled")) {
        if (detail) *detail = "Secure Boot enabled";
        return true;
    }
    if (exitCode == 0 && lower.contains("disabled")) {
        if (detail) *detail = "Secure Boot disabled";
        return false;
    }
    if (detail) *detail = "mokutil unavailable or sb-state could not be read";
    return false;
}

bool secureBootEnabled(QString *detail) {
    return secureBootFromMokutil(detail);
}

bool displayManagerActive(QString *detail) {
    QString dmDetail;
    const bool active = serviceActive("display-manager", &dmDetail)
        || serviceActive("gdm", &dmDetail)
        || serviceActive("sddm", &dmDetail)
        || serviceActive("lightdm", &dmDetail);
    if (detail) *detail = active ? ("display manager active: " + dmDetail) : "no display manager service active";
    return active;
}

bool sshdActive(QString *detail) {
    return serviceActive("sshd", detail) || serviceActive("ssh", detail);
}

QStringList companionAudioFunctions(const QString &gpuBdf, QString *detail) {
    const QString normalized = normalizeBdf(gpuBdf);
    QStringList companions;
    const QString group = groupNameForDevice(normalized);
    if (group.isEmpty()) {
        if (detail) *detail = "No IOMMU group available for companion lookup";
        return companions;
    }
    const auto members = devicesInIommuGroup(group, nullptr);
    for (const auto &dev : members) {
        if (normalizeBdf(dev) == normalized) continue;
        const QString cls = readText(sysfsPath(dev) + "/class");
        if (cls.startsWith("0x0403", Qt::CaseInsensitive) || cls.startsWith("0x0401", Qt::CaseInsensitive)) {
            companions.push_back(normalizeBdf(dev));
        }
    }
    if (detail) *detail = companions.isEmpty() ? "No companion audio functions found" : companions.join(", ");
    return companions;
}

static bool vmXmlHasHiddenKvmState(const QString &xml) {
    QXmlStreamReader xr(xml);
    bool inFeatures = false;
    bool inKvm = false;
    while (!xr.atEnd()) {
        xr.readNext();
        if (xr.isStartElement()) {
            const auto name = xr.name().toString();
            if (name == "features") inFeatures = true;
            else if (inFeatures && name == "kvm") inKvm = true;
            else if (inFeatures && inKvm && name == "hidden") {
                const auto state = xr.attributes().value("state").toString().toLower();
                if (state == "on" || state == "yes" || state == "true") return true;
            }
        } else if (xr.isEndElement()) {
            const auto name = xr.name().toString();
            if (name == "kvm") inKvm = false;
            else if (name == "features") inFeatures = false;
        }
    }
    return false;
}

static QString describePciDevice(const QString &bdf) {
    int exitCode = 0;
    const QString out = runCommandCapture("lspci", {"-nn", "-s", normalizeBdf(bdf)}, 5000, &exitCode);
    const QString line = out.section('\n', 0, 0).trimmed();
    if (exitCode == 0 && !line.isEmpty()) return line;
    const DeviceInfo d = inspectDevice(bdf);
    return QString("%1 vendor=%2 device=%3 class=%4")
        .arg(normalizeBdf(bdf))
        .arg(d.vendorId)
        .arg(d.deviceId)
        .arg(d.classCode);
}

QVariantMap systemInventoryToMap() {
    QVariantMap inv;
    QString detail;
    const AppConfig cfg = loadConfig();
    inv["kernelCmdline"] = kernelCmdline(&detail);
    inv["secureBootEnabled"] = secureBootEnabled(&detail);
    inv["displayManagerActive"] = displayManagerActive(&detail);
    inv["sshdActive"] = sshdActive(&detail);
    inv["iommuEnabled"] = iommuEnabled(&detail);
    inv["hookInstalled"] = hookInstalled(&detail);
    inv["fallbackDisplay"] = systemHasFallbackDisplay(&detail);
    inv["currentMode"] = cfg.mode;
    inv["preferredBoot"] = cfg.preferredBoot;
    inv["nextBootMode"] = cfg.nextBootMode;
    inv["autoStartVmOnBoot"] = cfg.autoStartVmOnBoot;
    inv["vmStoppedAwaitingDecision"] = cfg.vmStoppedAwaitingDecision;
    inv["lastStoppedVm"] = cfg.lastStoppedVm;
    inv["lastStoppedAt"] = cfg.lastStoppedAt;
    QVariantList devices;
    for (const auto &bdf : allGraphicsControllers(nullptr)) {
        const DeviceInfo d = inspectDevice(bdf);
        QVariantMap dm = deviceInfoToMap(d);
        dm["description"] = describePciDevice(bdf);
        QString compDetail;
        const auto audio = companionAudioFunctions(bdf, &compDetail);
        dm["audioCompanions"] = audio;
        devices.push_back(dm);
    }
    inv["graphicsDevices"] = devices;
    return inv;
}



QString renderInventoryText(const QVariantMap &inventory) {
    QString out;
    QTextStream ts(&out);
    ts << "Inventory\n";
    ts << "=========\n";
    ts << "Kernel cmdline: " << inventory.value("kernelCmdline").toString() << "\n";
    ts << "Secure Boot: " << (inventory.value("secureBootEnabled").toBool() ? "enabled" : "disabled/unknown") << "\n";
    ts << "Display manager: " << (inventory.value("displayManagerActive").toBool() ? "active" : "inactive") << "\n";
    ts << "SSH: " << (inventory.value("sshdActive").toBool() ? "active" : "inactive") << "\n";
    ts << "IOMMU groups: " << (inventory.value("iommuEnabled").toBool() ? "present" : "missing") << "\n";
    ts << "Libvirt hook: " << (inventory.value("hookInstalled").toBool() ? "installed" : "missing") << "\n";
    ts << "Fallback display: " << (inventory.value("fallbackDisplay").toBool() ? "available" : "not confirmed") << "\n";
    ts << "Current mode: " << inventory.value("currentMode").toString() << "\n";
    ts << "Preferred boot: " << inventory.value("preferredBoot").toString() << "\n";
    ts << "Next boot target: " << (inventory.value("nextBootMode").toString().isEmpty() ? "(none)" : inventory.value("nextBootMode").toString()) << "\n";
    ts << "Auto-start VM on boot: " << (inventory.value("autoStartVmOnBoot").toBool() ? "enabled" : "disabled") << "\n";
    ts << "VM stopped awaiting decision: " << (inventory.value("vmStoppedAwaitingDecision").toBool() ? "yes" : "no") << "\n";
    if (!inventory.value("lastStoppedVm").toString().isEmpty()) {
        ts << "Last stopped VM: " << inventory.value("lastStoppedVm").toString() << " at " << inventory.value("lastStoppedAt").toString() << "\n";
    }
    const auto devices = inventory.value("graphicsDevices").toList();
    ts << "\nGraphics devices:\n";
    for (const auto &v : devices) {
        const auto m = v.toMap();
        ts << " - " << m.value("bdf").toString() << " [" << m.value("vendor").toString() << "] "
           << m.value("description").toString() << "\n";
        ts << "   driver=" << m.value("driverName").toString()
           << " group=" << m.value("iommuGroup").toString()
           << " resetNode=" << (m.value("hasResetNode").toBool() ? "yes" : "no")
           << " temp=" << (m.value("temperatureReadable").toBool() ? QString::number(m.value("temperatureC").toInt()) + " C" : "unreadable") << "\n";
        const auto companions = m.value("audioCompanions").toList();
        if (!companions.isEmpty()) {
            QStringList c;
            for (const auto &a : companions) c << a.toString();
            ts << "   audio companions: " << c.join(", ") << "\n";
        }
    }
    return out;
}

QString renderPreflightText(const PreflightReport &r) {
    QString out;
    QTextStream ts(&out);
    ts << "Preflight report\n";
    ts << "=================\n";
    ts << "GPU: " << r.config.gpuBdf << "\n";
    ts << "Audio: " << (r.config.audioBdf.isEmpty() ? "(none)" : r.config.audioBdf) << "\n";
    ts << "Vendor: " << r.vendorName << "\n";
    ts << "Ready for VM: " << (r.canEnterVm ? "yes" : "no") << "\n";
    ts << "Next boot target: " << (r.config.nextBootMode.isEmpty() ? "(none)" : r.config.nextBootMode) << "\n";
    ts << "IOMMU: " << (r.compatibility.iommuEnabled ? "enabled" : "missing") << "\n";
    ts << "Group safe: " << (r.compatibility.groupSafe ? "yes" : "no") << "\n";
    ts << "Reset node: " << (r.compatibility.resetAvailable ? "yes" : "no") << "\n";
    ts << "Thermal guard: " << (r.compatibility.thermalGuardEnabled ? "enabled" : "disabled") << "\n";
    ts << "GPU temperature: " << (r.compatibility.gpuTemperatureReadable ? QString::number(r.compatibility.gpuTemperatureC) + " C" : "unreadable") << "\n";
    ts << "Temperature limit: " << r.compatibility.maxGpuTempC << " C" << "\n";
    ts << "VM-stop auto recovery: " << r.compatibility.safetyAutoRecoveryMinutes << " minute(s)" << "\n";
    ts << "Vendor reset: " << (r.compatibility.vendorResetAvailable ? "available" : "not available") << "\n";
    ts << "Libvirt hook: " << (r.compatibility.hookInstalled ? "installed" : "missing") << "\n";
    ts << "Fallback display/SSH: " << (r.compatibility.hasFallbackDisplay || r.compatibility.sshdActive ? "yes" : "no") << "\n";
    ts << "Secure Boot: " << (r.compatibility.secureBootEnabled ? "enabled" : "disabled/unknown") << "\n";
    ts << "NVIDIA hidden-state: " << (r.compatibility.nvidiaHiddenState ? "present" : "not detected") << "\n\n";
    if (!r.blockers.isEmpty()) {
        ts << "Blockers:\n";
        for (const auto &b : r.blockers) ts << " - " << b << "\n";
    }
    if (!r.warnings.isEmpty()) {
        ts << "Warnings:\n";
        for (const auto &w : r.warnings) ts << " - " << w << "\n";
    }
    if (!r.notes.isEmpty()) {
        ts << "Notes:\n";
        for (const auto &n : r.notes) ts << " - " << n << "\n";
    }
    return out;
}

QString renderPreflightText(const QVariantMap &preflight) {
    QString out;
    QTextStream ts(&out);
    ts << "Preflight report\n";
    ts << "=================\n";
    const auto cfg = preflight.value("config").toMap();
    const auto comp = preflight.value("compatibility").toMap();
    const QString nextBoot = cfg.value("nextBootMode").toString();
    ts << "GPU: " << cfg.value("gpuBdf").toString() << "\n";
    ts << "Audio: " << (cfg.value("audioBdf").toString().isEmpty() ? "(none)" : cfg.value("audioBdf").toString()) << "\n";
    ts << "Vendor: " << preflight.value("vendorName").toString() << "\n";
    ts << "Ready for VM: " << (preflight.value("canEnterVm").toBool() ? "yes" : "no") << "\n";
    ts << "Next boot target: " << (nextBoot.isEmpty() ? "(none)" : nextBoot) << "\n";
    ts << "IOMMU: " << (comp.value("iommuEnabled").toBool() ? "enabled" : "missing") << "\n";
    ts << "Group safe: " << (comp.value("groupSafe").toBool() ? "yes" : "no") << "\n";
    ts << "Reset node: " << (comp.value("resetAvailable").toBool() ? "yes" : "no") << "\n";
    ts << "Thermal guard: " << (comp.value("thermalGuardEnabled").toBool() ? "enabled" : "disabled") << "\n";
    ts << "GPU temperature: " << (comp.value("gpuTemperatureReadable").toBool() ? QString::number(comp.value("gpuTemperatureC").toInt()) + " C" : "unreadable") << "\n";
    ts << "Temperature limit: " << comp.value("maxGpuTempC").toInt() << " C" << "\n";
    ts << "VM-stop auto recovery: " << comp.value("safetyAutoRecoveryMinutes").toInt() << " minute(s)" << "\n";
    ts << "Vendor reset: " << (comp.value("vendorResetAvailable").toBool() ? "available" : "not available") << "\n";
    ts << "Libvirt hook: " << (comp.value("hookInstalled").toBool() ? "installed" : "missing") << "\n";
    ts << "Fallback display/SSH: " << ((comp.value("hasFallbackDisplay").toBool() || comp.value("sshdActive").toBool()) ? "yes" : "no") << "\n";
    ts << "Secure Boot: " << (comp.value("secureBootEnabled").toBool() ? "enabled" : "disabled/unknown") << "\n";
    ts << "NVIDIA hidden-state: " << (comp.value("nvidiaHiddenState").toBool() ? "present" : "not detected") << "\n\n";
    const auto blockers = preflight.value("blockers").toList();
    const auto warnings = preflight.value("warnings").toList();
    const auto notes = preflight.value("notes").toList();
    if (!blockers.isEmpty()) {
        ts << "Blockers:\n";
        for (const auto &v : blockers) ts << " - " << v.toString() << "\n";
    }
    if (!warnings.isEmpty()) {
        ts << "Warnings:\n";
        for (const auto &v : warnings) ts << " - " << v.toString() << "\n";
    }
    if (!notes.isEmpty()) {
        ts << "Notes:\n";
        for (const auto &v : notes) ts << " - " << v.toString() << "\n";
    }
    return out;
}

} // namespace gs
