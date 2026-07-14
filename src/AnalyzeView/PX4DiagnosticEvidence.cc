/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "PX4DiagnosticEvidence.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>

namespace {

QString listenerOutputField(const QString &output, const QString &fieldName)
{
    const QRegularExpression expression(
        QStringLiteral("(?m)^\\s*%1\\s*:\\s*([^\\r\\n]*)$").arg(QRegularExpression::escape(fieldName)));
    const QRegularExpressionMatch match = expression.match(output);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString fieldState(const QString &value)
{
    if (value.isEmpty()) {
        return QStringLiteral("unknown");
    }

    return value.contains(QStringLiteral("nan"), Qt::CaseInsensitive)
        ? QStringLiteral("invalid")
        : QStringLiteral("confirmed");
}

bool listenerOutputUnavailable(const QString &output)
{
    return output.contains(QStringLiteral("unknown topic"), Qt::CaseInsensitive) ||
        output.contains(QStringLiteral("not found"), Qt::CaseInsensitive) ||
        output.contains(QStringLiteral("not available"), Qt::CaseInsensitive) ||
        output.contains(QStringLiteral("not advertised"), Qt::CaseInsensitive);
}

}

QJsonObject PX4DiagnosticEvidence::_baseEvidence(const QString &state, const QString &source, const QJsonObject &observations)
{
    QJsonObject evidence;
    evidence.insert(QStringLiteral("state"), state);
    evidence.insert(QStringLiteral("source"), source);
    evidence.insert(QStringLiteral("capturedAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    evidence.insert(QStringLiteral("observations"), observations);
    return evidence;
}

QJsonObject PX4DiagnosticEvidence::_unknownEvidence(const QString &source, const QString &reason)
{
    QJsonObject observations;
    observations.insert(QStringLiteral("reason"), reason);
    return _baseEvidence(QStringLiteral("unknown"), source, observations);
}

QJsonObject PX4DiagnosticEvidence::_unavailableEvidence(const QString &source, const QString &reason)
{
    QJsonObject observations;
    observations.insert(QStringLiteral("reason"), reason);
    return _baseEvidence(QStringLiteral("unavailable"), source, observations);
}

QJsonObject PX4DiagnosticEvidence::_externalVisionInputEvidence(const QString &output, bool outputTruncated)
{
    const QString source = QStringLiteral("PX4 shell: listener vehicle_visual_odometry 1");
    if (listenerOutputUnavailable(output)) {
        return _unavailableEvidence(source, QStringLiteral("PX4 did not provide the vehicle_visual_odometry topic."));
    }

    const bool topicSeen = output.contains(QStringLiteral("TOPIC: vehicle_visual_odometry"), Qt::CaseInsensitive);
    const QString timestamp = listenerOutputField(output, QStringLiteral("timestamp"));
    const QString position = listenerOutputField(output, QStringLiteral("position"));
    const QString quaternion = listenerOutputField(output, QStringLiteral("q"));
    const QString velocity = listenerOutputField(output, QStringLiteral("velocity"));

    QJsonObject observations;
    observations.insert(QStringLiteral("topicSeen"), topicSeen);
    observations.insert(QStringLiteral("timestampSeen"), !timestamp.isEmpty());
    observations.insert(QStringLiteral("positionState"), fieldState(position));
    observations.insert(QStringLiteral("quaternionState"), fieldState(quaternion));
    observations.insert(QStringLiteral("velocityState"), fieldState(velocity));
    observations.insert(QStringLiteral("velocityProvided"), !velocity.isEmpty());
    observations.insert(QStringLiteral("rawObservationAvailable"), !output.trimmed().isEmpty());
    observations.insert(QStringLiteral("outputTruncated"), outputTruncated);

    const QRegularExpression ageExpression(QStringLiteral("\\(([0-9]+(?:\\.[0-9]+)?)\\s+seconds?\\s+ago\\)"));
    const QRegularExpressionMatch ageMatch = ageExpression.match(timestamp);
    if (ageMatch.hasMatch()) {
        observations.insert(QStringLiteral("timestampAgeSeconds"), ageMatch.captured(1).toDouble());
    }

    QString state = QStringLiteral("unknown");
    if (fieldState(position) == QStringLiteral("invalid") || fieldState(quaternion) == QStringLiteral("invalid")) {
        state = QStringLiteral("invalid");
    } else if (topicSeen && !timestamp.isEmpty() && fieldState(position) == QStringLiteral("confirmed") && fieldState(quaternion) == QStringLiteral("confirmed")) {
        state = QStringLiteral("confirmed");
    }

    return _baseEvidence(state, source, observations);
}

QJsonObject PX4DiagnosticEvidence::_externalVisionFusionEvidence(const QString &output, bool outputTruncated)
{
    const QString source = QStringLiteral("PX4 shell: listener estimator_status_flags 1");
    if (listenerOutputUnavailable(output)) {
        return _unavailableEvidence(source, QStringLiteral("PX4 did not provide estimator_status_flags."));
    }

    QJsonObject observations;
    observations.insert(QStringLiteral("rawObservationAvailable"), !output.trimmed().isEmpty());
    observations.insert(QStringLiteral("outputTruncated"), outputTruncated);

    bool anyFlagFound = false;
    const QStringList flags{
        QStringLiteral("cs_ev_pos"),
        QStringLiteral("cs_ev_vel"),
        QStringLiteral("cs_ev_yaw"),
        QStringLiteral("cs_ev_hgt"),
    };

    for (const QString &flag : flags) {
        const QRegularExpression expression(
            QStringLiteral("(?m)^\\s*%1\\s*:\\s*(true|false|1|0)\\s*$").arg(QRegularExpression::escape(flag)),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = expression.match(output);
        if (match.hasMatch()) {
            const QString value = match.captured(1).toLower();
            observations.insert(flag, value == QStringLiteral("true") || value == QStringLiteral("1"));
            anyFlagFound = true;
        }
    }

    if (!anyFlagFound) {
        return _unknownEvidence(source, QStringLiteral("No external-vision fusion-control fields were found in the console output."));
    }

    observations.insert(QStringLiteral("semanticBoundary"), QStringLiteral("These flags describe EKF fusion control state; they do not by themselves prove every measurement was accepted."));
    return _baseEvidence(QStringLiteral("confirmed"), source, observations);
}

QJsonObject PX4DiagnosticEvidence::_externalVisionConfigurationEvidence(const PX4ExternalVisionSnapshot &snapshot, const QString &source)
{
    QJsonObject observations;
    observations.insert(QStringLiteral("parameterManagerReady"), snapshot.parameterManagerReady);
    observations.insert(QStringLiteral("ekf2EvCtrlAvailable"), snapshot.evCtrlAvailable);
    observations.insert(QStringLiteral("ekf2HgtRefAvailable"), snapshot.hgtRefAvailable);

    if (!snapshot.evCtrlAvailable) {
        return _unavailableEvidence(source, QStringLiteral("EKF2_EV_CTRL is not available from the loaded PX4 parameters."));
    }

    observations.insert(QStringLiteral("ekf2EvCtrl"), snapshot.evCtrl);
    observations.insert(QStringLiteral("horizontalPositionFusionEnabled"), static_cast<bool>(snapshot.evCtrl & (1 << 0)));
    observations.insert(QStringLiteral("verticalPositionFusionEnabled"), static_cast<bool>(snapshot.evCtrl & (1 << 1)));
    observations.insert(QStringLiteral("velocityFusionEnabled"), static_cast<bool>(snapshot.evCtrl & (1 << 2)));
    observations.insert(QStringLiteral("yawFusionEnabled"), static_cast<bool>(snapshot.evCtrl & (1 << 3)));
    if (snapshot.hgtRefAvailable) {
        observations.insert(QStringLiteral("ekf2HgtRef"), snapshot.hgtRef);
    }

    return _baseEvidence(QStringLiteral("confirmed"), source, observations);
}

QJsonObject PX4DiagnosticEvidence::_externalVisionConfigurationEvidenceFromConsole(const QString &output, bool outputTruncated)
{
    const QString source = QStringLiteral("PX4 shell: param show EKF2_EV_CTRL");
    if (listenerOutputUnavailable(output)) {
        return _unavailableEvidence(source, QStringLiteral("PX4 did not provide EKF2_EV_CTRL."));
    }

    const QRegularExpression expression(QStringLiteral("(?m)\\bEKF2_EV_CTRL\\b[^\\r\\n]*?:\\s*(-?[0-9]+)\\s*$"));
    const QRegularExpressionMatch match = expression.match(output);
    if (!match.hasMatch()) {
        return _unknownEvidence(source, outputTruncated
            ? QStringLiteral("The EKF2_EV_CTRL console output was truncated before a value could be parsed.")
            : QStringLiteral("No EKF2_EV_CTRL value could be parsed from the console output."));
    }

    PX4ExternalVisionSnapshot snapshot;
    snapshot.parameterManagerReady = true;
    snapshot.evCtrlAvailable = true;
    snapshot.evCtrl = match.captured(1).toInt();
    QJsonObject evidence = _externalVisionConfigurationEvidence(snapshot, source);
    QJsonObject observations = evidence.value(QStringLiteral("observations")).toObject();
    observations.insert(QStringLiteral("outputTruncated"), outputTruncated);
    observations.insert(QStringLiteral("rawObservationAvailable"), !output.trimmed().isEmpty());
    evidence.insert(QStringLiteral("observations"), observations);
    return evidence;
}

QJsonObject PX4DiagnosticEvidence::externalVisionSnapshot(const PX4ExternalVisionSnapshot &snapshot)
{
    QJsonObject sysStatusObservations;
    sysStatusObservations.insert(QStringLiteral("present"), snapshot.sysStatusPresent);
    sysStatusObservations.insert(QStringLiteral("enabled"), snapshot.sysStatusEnabled);
    sysStatusObservations.insert(QStringLiteral("healthy"), snapshot.sysStatusHealthy);
    sysStatusObservations.insert(QStringLiteral("semanticBoundary"), QStringLiteral("SYS_STATUS enabled and health bits do not prove whether vehicle_visual_odometry messages are arriving or being fused."));

    QJsonObject externalVision;
    externalVision.insert(QStringLiteral("input"), _unknownEvidence(
        QStringLiteral("PX4 shell: listener vehicle_visual_odometry 1"),
        QStringLiteral("No external-vision input probe has been run for this snapshot.")));
    externalVision.insert(QStringLiteral("configuration"), _externalVisionConfigurationEvidence(
        snapshot, QStringLiteral("QGroundControl PX4 ParameterManager")));
    externalVision.insert(QStringLiteral("fusionControl"), _unknownEvidence(
        QStringLiteral("PX4 shell: listener estimator_status_flags 1"),
        QStringLiteral("No external-vision fusion-control probe has been run for this snapshot.")));
    externalVision.insert(QStringLiteral("sysStatus"), snapshot.sysStatusAvailable
        ? _baseEvidence(QStringLiteral("confirmed"), QStringLiteral("MAVLink SYS_STATUS"), sysStatusObservations)
        : _unavailableEvidence(QStringLiteral("MAVLink SYS_STATUS"), QStringLiteral("No SYS_STATUS observation is available.")));

    QJsonObject navigationObservations;
    navigationObservations.insert(QStringLiteral("gpsFactGroup"), QStringLiteral("factGroups.gps"));
    navigationObservations.insert(QStringLiteral("localPositionFactGroup"), QStringLiteral("factGroups.localPosition"));
    navigationObservations.insert(QStringLiteral("estimatorStatusFactGroup"), QStringLiteral("factGroups.estimatorStatus"));
    navigationObservations.insert(QStringLiteral("semanticBoundary"), QStringLiteral("GPS and global-position availability do not determine whether external-vision input is present. ESTIMATOR_POS_HORIZ_ABS does not identify the aiding source."));
    externalVision.insert(QStringLiteral("navigation"), _baseEvidence(
        QStringLiteral("confirmed"), QStringLiteral("QGroundControl FactGroups"), navigationObservations));

    QJsonArray semanticConstraints;
    semanticConstraints.append(QStringLiteral("Never infer missing external-vision input solely from SYS_STATUS Computer vision position Disabled."));
    semanticConstraints.append(QStringLiteral("Never infer external-vision availability from GPS lock or global-position status."));
    semanticConstraints.append(QStringLiteral("Do not report a publication rate unless a measurement window is available."));
    semanticConstraints.append(QStringLiteral("Use only EKF2_EV_CTRL as the PX4 external-vision configuration parameter unless another parameter is directly observed."));

    QJsonObject result;
    result.insert(QStringLiteral("schemaVersion"), 1);
    result.insert(QStringLiteral("externalVision"), externalVision);
    result.insert(QStringLiteral("semanticConstraints"), semanticConstraints);
    return result;
}

QJsonObject PX4DiagnosticEvidence::externalVisionConsoleEvidence(const QString &command, const QString &output, bool outputTruncated)
{
    QJsonObject externalVision;
    if (command == externalVisionConsoleCommands().at(0)) {
        externalVision.insert(QStringLiteral("input"), _externalVisionInputEvidence(output, outputTruncated));
    } else if (command == externalVisionConsoleCommands().at(1)) {
        externalVision.insert(QStringLiteral("fusionControl"), _externalVisionFusionEvidence(output, outputTruncated));
    } else if (command == externalVisionConsoleCommands().at(2)) {
        externalVision.insert(QStringLiteral("configuration"), _externalVisionConfigurationEvidenceFromConsole(output, outputTruncated));
    }

    if (externalVision.isEmpty()) {
        return QJsonObject();
    }

    QJsonObject result;
    result.insert(QStringLiteral("schemaVersion"), 1);
    result.insert(QStringLiteral("externalVision"), externalVision);
    return result;
}

QJsonObject PX4DiagnosticEvidence::externalVisionUnavailableEvidence(const QString &command, const QString &reason)
{
    QJsonObject externalVision;
    if (command == externalVisionConsoleCommands().at(0)) {
        externalVision.insert(QStringLiteral("input"), _unavailableEvidence(
            QStringLiteral("PX4 shell: listener vehicle_visual_odometry 1"), reason));
    } else if (command == externalVisionConsoleCommands().at(1)) {
        externalVision.insert(QStringLiteral("fusionControl"), _unavailableEvidence(
            QStringLiteral("PX4 shell: listener estimator_status_flags 1"), reason));
    } else if (command == externalVisionConsoleCommands().at(2)) {
        externalVision.insert(QStringLiteral("configuration"), _unavailableEvidence(
            QStringLiteral("PX4 shell: param show EKF2_EV_CTRL"), reason));
    }

    if (externalVision.isEmpty()) {
        return QJsonObject();
    }

    QJsonObject result;
    result.insert(QStringLiteral("schemaVersion"), 1);
    result.insert(QStringLiteral("externalVision"), externalVision);
    return result;
}

bool PX4DiagnosticEvidence::isExternalVisionQuestion(const QString &question)
{
    const QString normalized = question.toLower();
    static const QStringList needles{
        QStringLiteral("视觉"),
        QStringLiteral("vio"),
        QStringLiteral("视觉里程计"),
        QStringLiteral("外部视觉"),
        QStringLiteral("external vision"),
        QStringLiteral("vicon"),
        QStringLiteral("mocap"),
        QStringLiteral("odometry"),
        QStringLiteral("vehicle_visual_odometry"),
        QStringLiteral("vision position"),
    };

    for (const QString &needle : needles) {
        if (normalized.contains(needle)) {
            return true;
        }
    }

    return false;
}

QStringList PX4DiagnosticEvidence::externalVisionConsoleCommands()
{
    return {
        QStringLiteral("listener vehicle_visual_odometry 1"),
        QStringLiteral("listener estimator_status_flags 1"),
        QStringLiteral("param show EKF2_EV_CTRL"),
    };
}

QString PX4DiagnosticEvidence::externalVisionInputSensorType()
{
    return QStringLiteral("external_vision_input");
}

QString PX4DiagnosticEvidence::externalVisionFusionSensorType()
{
    return QStringLiteral("external_vision_fusion");
}

PX4DiagnosticEvidence::ConsoleQueryAvailability PX4DiagnosticEvidence::consoleQueryAvailability(bool armed, bool flying, bool communicationLost)
{
    if (communicationLost) {
        return ConsoleQueryAvailability::CommunicationLost;
    }
    if (armed || flying) {
        return ConsoleQueryAvailability::ArmedOrFlying;
    }
    return ConsoleQueryAvailability::Allowed;
}
