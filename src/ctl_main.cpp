#include "common.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QElapsedTimer>
#include <QThread>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace gs;

static bool runningAsRoot() {
#ifdef Q_OS_UNIX
    return ::geteuid() == 0;
#else
    return false;
#endif
}

static QString helperExecutableForCli() {
    const QFileInfo installed(helperBinaryPath());
    if (installed.isExecutable()) return installed.absoluteFilePath();

    const QString sibling = QCoreApplication::applicationDirPath() + "/gpu-switcher-helperd";
    const QFileInfo local(sibling);
    if (local.isExecutable()) return local.absoluteFilePath();

    return helperBinaryPath();
}

static bool waitForHelperSocket(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QLocalSocket probe;
        probe.connectToServer(socketPath());
        if (probe.waitForConnected(250)) {
            probe.disconnectFromServer();
            return true;
        }
        QThread::msleep(150);
    }
    return false;
}

static bool startHelperForSudo(QString *err) {
    if (!runningAsRoot()) {
        if (err) {
            *err = "Could not connect to helper at " + socketPath()
                 + ". Start it with: sudo systemctl enable --now gpu-switcher-helperd.service. "
                   "Or run this CLI command with sudo so it can auto-start the helper.";
        }
        return false;
    }

    const QString helper = helperExecutableForCli();
    if (!QFileInfo(helper).isExecutable()) {
        if (err) {
            *err = "Could not connect to helper at " + socketPath()
                 + ", and helper binary is not executable at " + helper;
        }
        return false;
    }

    if (!QProcess::startDetached(helper, QStringList{})) {
        if (err) *err = "Could not start helper daemon: " + helper;
        return false;
    }

    if (!waitForHelperSocket(5000)) {
        if (err) {
            *err = "Started helper daemon at " + helper
                 + ", but no helper socket appeared at " + socketPath();
        }
        return false;
    }
    return true;
}

static bool requestHelper(const QJsonObject &req, QJsonObject *resp, QString *err) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        QLocalSocket sock;
        sock.connectToServer(socketPath());
        if (!sock.waitForConnected(3000)) {
            if (attempt == 0) {
                QString startErr;
                if (startHelperForSudo(&startErr)) continue;
                if (err) *err = startErr;
                return false;
            }
            if (err) *err = "Could not connect to helper at " + socketPath();
            return false;
        }

        const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
        sock.write(line);
        if (!sock.waitForBytesWritten(3000)) {
            if (err) *err = "Failed to send request";
            return false;
        }
        if (!sock.waitForReadyRead(3000)) {
            if (err) *err = "No response from helper";
            return false;
        }
        const QByteArray respLine = sock.readLine().trimmed();
        const auto doc = QJsonDocument::fromJson(respLine);
        if (!doc.isObject()) {
            if (err) *err = "Invalid helper response";
            return false;
        }
        const QJsonObject obj = doc.object();
        if (!obj.value("ok").toBool()) {
            if (err) *err = obj.value("error").toString("Helper error");
            return false;
        }
        if (resp) *resp = obj;
        return true;
    }

    if (err) *err = "Could not connect to helper at " + socketPath();
    return false;
}

static int printJson(const QJsonObject &obj) {
    qInfo().noquote() << QJsonDocument(obj).toJson(QJsonDocument::Indented);
    return 0;
}

static int printTextForCommand(const QString &cmd, const QJsonObject &obj) {
    if (cmd == "inventory") {
        const auto inventory = obj.value("inventory").toObject().toVariantMap();
        qInfo().noquote() << renderInventoryText(inventory);
        return 0;
    }
    if (cmd == "diagnose" || cmd == "preflight") {
        const auto diag = obj.value("diagnostic").toObject().toVariantMap();
        qInfo().noquote() << renderPreflightText(diag);
        return 0;
    }
    if (cmd == "simulate" || cmd == "simulateVm" || cmd == "simulateHost") {
        const auto sim = obj.value("simulation").toObject();
        const auto preflight = sim.value("preflight").toObject().toVariantMap();
        qInfo().noquote() << "Simulation report";
        qInfo().noquote() << "==================";
        qInfo().noquote() << "Flow:" << sim.value("flow").toString();
        qInfo().noquote() << "State:" << sim.value("state").toString();
        qInfo().noquote() << "Phases:" << sim.value("phases").toArray().size();
        qInfo().noquote() << "";
        qInfo().noquote() << renderPreflightText(preflight);
        return 0;
    }
    return printJson(obj);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 2) {
        qCritical() << "Usage: gsc <status|probe|inventory|diagnose|autoSetupSingleGpu|simulate|simulateVm|simulateHost|installHook|switchToVm|switchToHost|rebootToVm|rebootToHost|restartHostNow|returnHostNextRestart|keepGpuForVm|safetyRecoverHostNow|applyBootState|on-vm-stopped> [gpuBdf] [--json]";
        return 1;
    }

    const QString cmd = args.at(1);
    const bool jsonOut = args.contains("--json");
    const bool hasGpuArg = args.size() >= 3 && !args.at(2).startsWith("--");
    const QString gpuBdf = hasGpuArg ? args.at(2) : QString();

    QJsonObject req;
    if (cmd == "autoSetupSingleGpu") {
        req = QJsonObject{{"cmd", cmd}};
        if (!gpuBdf.isEmpty()) req["gpuBdf"] = gpuBdf;
    } else if (cmd == "status" || cmd == "installHook" || cmd == "switchToVm" || cmd == "switchToHost" || cmd == "rebootToVm" || cmd == "rebootToHost" || cmd == "restartHostNow" || cmd == "returnHostNextRestart" || cmd == "keepGpuForVm" || cmd == "safetyRecoverHostNow" || cmd == "applyBootState") {
        req = QJsonObject{{"cmd", cmd}};
    } else if (cmd == "probe") {
        if (gpuBdf.isEmpty()) {
            qCritical() << "Usage: gsc probe <gpuBdf>";
            return 1;
        }
        req = QJsonObject{{"cmd", "probe"}, {"gpuBdf", gpuBdf}};
    } else if (cmd == "diagnose" || cmd == "preflight") {
        req = QJsonObject{{"cmd", "diagnose"}};
        if (!gpuBdf.isEmpty()) req["gpuBdf"] = gpuBdf;
    } else if (cmd == "inventory") {
        req = QJsonObject{{"cmd", "inventory"}};
    } else if (cmd == "simulate" || cmd == "simulateVm" || cmd == "simulateHost") {
        req = QJsonObject{{"cmd", cmd}};
        if (!gpuBdf.isEmpty()) req["gpuBdf"] = gpuBdf;
    } else if (cmd == "on-vm-stopped") {
        const QString domain = hasGpuArg ? args.at(2) : QString();
        req = QJsonObject{{"cmd", "onVmStopped"}, {"domain", domain}};
    } else {
        qCritical() << "Unknown command:" << cmd;
        return 1;
    }

    QJsonObject resp;
    QString err;
    if (!requestHelper(req, &resp, &err)) {
        qCritical() << err;
        return 2;
    }

    if (jsonOut || (cmd != "inventory" && cmd != "diagnose" && cmd != "preflight" && !cmd.startsWith("simulate"))) {
        return printJson(resp);
    }
    return printTextForCommand(cmd, resp);
}
