#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <QList>
#include <optional>

namespace gs {

enum class VendorKind {
    Unknown,
    AMD,
    NVIDIA,
    Intel,
    Other
};

enum class DiagnosticSeverity {
    Info,
    Warning,
    Blocker
};

struct AppConfig {
    QString vmName;
    QString vmUuid;
    QString gpuBdf;
    QString audioBdf;
    QString preferredBoot = "host";
    QString nextBootMode;
    bool useVendorReset = false;
    bool hasFallbackDisplay = false;
    bool allowSingleGpu = false;
    bool autoStartVmOnBoot = true;
    bool thermalGuardEnabled = true;
    int maxGpuTempC = 85;
    int safetyAutoRecoveryMinutes = 10;
    QString safetyRecoveryDeadline;
    bool vmStoppedAwaitingDecision = false;
    QString lastStoppedVm;
    QString lastStoppedAt;
    QString mode = "host";
    QString originalGpuDriver;
    QString originalAudioDriver;
};

struct DeviceInfo {
    QString bdf;
    QString vendorId;
    QString deviceId;
    QString classCode;
    QString driverName;
    QString resetMethod;
    QString iommuGroup;
    bool hasResetNode = false;
    bool exists = false;
    bool temperatureReadable = false;
    int temperatureC = -1;
    QString temperaturePath;
};

struct LaptopSafetyInfo {
    bool chassisLooksPortable = false;
    bool batteryPresent = false;
    bool likelyLaptop = false;
    bool targetHasDrmCard = false;
    bool targetDrivesActiveDisplay = false;
    bool targetDrivesInternalPanel = false;
    bool targetHasExternalConnectedDisplay = false;
    bool targetIsOnlyGraphicsController = false;
    bool targetAppearsSafeHybridOffload = false;
    QString chassisType;
    QStringList targetDrmCards;
    QStringList activeConnectors;
    QStringList internalConnectors;
    QStringList externalConnectors;
    QString summary;
    QStringList blockers;
    QStringList warnings;
};

struct CompatibilityReport {
    bool iommuEnabled = false;
    bool gpuExists = false;
    bool groupSafe = false;
    bool hasFallbackDisplay = false;
    bool singleGraphicsPath = false;
    bool resetAvailable = false;
    bool vmXmlMatches = false;
    bool hookInstalled = false;
    bool vendorResetAvailable = false;
    bool secureBootEnabled = false;
    bool displayManagerActive = false;
    bool sshdActive = false;
    bool nvidiaHiddenState = false;
    bool gpuHasAudioCompanion = false;
    bool likelyLaptop = false;
    bool laptopTargetHasDrmCard = false;
    bool laptopTargetDrivesDisplay = false;
    bool laptopTargetDrivesInternalPanel = false;
    bool laptopTargetHasExternalConnectedDisplay = false;
    bool laptopTargetAppearsSafeHybridOffload = false;
    QString laptopChassisType;
    QString laptopSafetySummary;
    QStringList laptopTargetDrmCards;
    QStringList laptopActiveConnectors;
    QStringList laptopInternalConnectors;
    QStringList laptopExternalConnectors;
    QStringList laptopBlockers;
    QStringList laptopWarnings;
    bool gpuTemperatureReadable = false;
    int gpuTemperatureC = -1;
    int maxGpuTempC = 85;
    bool thermalGuardEnabled = true;
    int safetyAutoRecoveryMinutes = 10;
    QString safetyRecoveryDeadline;
    QString gpuTemperaturePath;
    QString gpuModel;
    QString kernelCmdline;
    QString vendorName;
    QString domainId;
    QStringList reasons;
    QStringList warnings;
};

struct PreflightFinding {
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    QString code;
    QString title;
    QString detail;
    QString remediation;
};

struct PreflightReport {
    AppConfig config;
    CompatibilityReport compatibility;
    VendorKind vendor = VendorKind::Unknown;
    QString vendorName;
    bool canEnterVm = false;
    QStringList stages;
    QStringList blockers;
    QStringList warnings;
    QStringList notes;
    QList<PreflightFinding> findings;
};

QString configPath();
QString statePath();
QString socketPath();
QString hookPath();
QString helperBinaryPath();
QString ctlBinaryPath();
QString backupDirPath();

bool ensureRuntimeDirs(QString *error = nullptr);
bool readFile(const QString &path, QString *out);
bool writeFile(const QString &path, const QString &data, QString *error = nullptr);
QString trim(const QString &s);
QString shellQuote(const QString &s);
QString runCommandCapture(const QString &program, const QStringList &args, int timeoutMs, int *exitCode = nullptr);
QString normalizeBdf(const QString &bdf);
bool isPciBdf(const QString &bdf);
QString domainIdForConfig(const AppConfig &cfg);
QString normalizeBootTarget(const QString &mode);

AppConfig loadConfig(QString *error = nullptr);
bool saveConfig(const AppConfig &cfg, QString *error = nullptr);
QString readState(QString *error = nullptr);
bool writeState(const QString &state, QString *error = nullptr);

bool iommuEnabled(QString *detail = nullptr);
QStringList allGraphicsControllers(QString *detail = nullptr);
bool systemHasFallbackDisplay(QString *detail = nullptr);
bool singleGPUOnly(QString gpuBdf, QString *detail = nullptr);
LaptopSafetyInfo inspectLaptopSafety(const QString &targetGpuBdf);
QStringList devicesInIommuGroup(QString groupName, QString *detail = nullptr);
bool iommuGroupIsSafe(const QString &gpuBdf, QStringList *reasons = nullptr);
QString kernelCmdline(QString *detail = nullptr);
bool secureBootEnabled(QString *detail = nullptr);
bool displayManagerActive(QString *detail = nullptr);
bool sshdActive(QString *detail = nullptr);
bool readGpuTemperatureC(const QString &bdf, int *temperatureC = nullptr, QString *sensorPath = nullptr, QString *detail = nullptr);
QStringList companionAudioFunctions(const QString &gpuBdf, QString *detail = nullptr);

DeviceInfo inspectDevice(const QString &bdf);
QStringList supportedResetMethods(const QString &bdf);
bool deviceVendorIsAmd(const DeviceInfo &d);
bool deviceVendorIsNvidia(const DeviceInfo &d);
VendorKind vendorKind(const DeviceInfo &d);
QString vendorKindName(VendorKind kind);
bool hookInstalled(QString *detail = nullptr);
bool vmXmlMatchesConfig(const AppConfig &cfg, QStringList *issues = nullptr, QString *rawXml = nullptr);
bool backupVmXml(const AppConfig &cfg, QString *backupPath = nullptr, QString *error = nullptr);
bool canUseVendorReset(const DeviceInfo &gpu, QString *detail = nullptr);
bool bestEffortReset(const QString &bdf, const AppConfig &cfg, QStringList *notes = nullptr, QString *error = nullptr);
bool pciReset(const QString &bdf, QString *error = nullptr);
bool unbindDriver(const QString &bdf, QString *error = nullptr);
bool bindToDriver(const QString &bdf, const QString &driver, QString *error = nullptr);
bool setDriverOverride(const QString &bdf, const QString &driver, QString *error = nullptr);
bool clearDriverOverride(const QString &bdf, QString *error = nullptr);
bool removeFromBus(const QString &bdf, QString *error = nullptr);
bool rescanBus(QString *error = nullptr);

QVariantMap deviceInfoToMap(const DeviceInfo &d);
QVariantMap reportToMap(const CompatibilityReport &r);
QVariantMap preflightToMap(const PreflightReport &r);
QVariantMap systemInventoryToMap();
CompatibilityReport buildCompatibilityReport(const AppConfig &cfg);
PreflightReport buildPreflightReport(const AppConfig &cfg);
QString renderPreflightText(const PreflightReport &r);
QString renderPreflightText(const QVariantMap &preflight);
QString renderInventoryText(const QVariantMap &inventory);

} // namespace gs
