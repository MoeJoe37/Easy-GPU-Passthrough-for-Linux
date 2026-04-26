
#include "common.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

using namespace gs;

static bool requestHelper(const QJsonObject &req, QJsonObject *resp, QString *err) {
    QLocalSocket sock;
    sock.connectToServer(socketPath());
    if (!sock.waitForConnected(3000)) {
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
        qCritical() << "Usage: gpu-switcher-ctl <status|probe|inventory|diagnose|autoSetupSingleGpu|simulate|simulateVm|simulateHost|installHook|switchToVm|switchToHost|rebootToVm|rebootToHost|restartHostNow|returnHostNextRestart|keepGpuForVm|safetyRecoverHostNow|applyBootState|on-vm-stopped> [gpuBdf] [--json]";
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
            qCritical() << "Usage: gpu-switcher-ctl probe <gpuBdf>";
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
