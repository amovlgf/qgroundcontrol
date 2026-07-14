/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

struct PX4ExternalVisionSnapshot {
    bool sysStatusAvailable = false;
    bool sysStatusPresent = false;
    bool sysStatusEnabled = false;
    bool sysStatusHealthy = false;

    bool parameterManagerReady = false;
    bool evCtrlAvailable = false;
    int evCtrl = 0;
    bool hgtRefAvailable = false;
    int hgtRef = 0;
};

class PX4DiagnosticEvidence
{
public:
    enum class ConsoleQueryAvailability {
        Allowed,
        ArmedOrFlying,
        CommunicationLost,
    };

    static QJsonObject externalVisionSnapshot(const PX4ExternalVisionSnapshot &snapshot);
    static QJsonObject externalVisionConsoleEvidence(const QString &command, const QString &output, bool outputTruncated);
    static QJsonObject externalVisionUnavailableEvidence(const QString &command, const QString &reason);

    static bool isExternalVisionQuestion(const QString &question);
    static QStringList externalVisionConsoleCommands();
    static QString externalVisionInputSensorType();
    static QString externalVisionFusionSensorType();

    static ConsoleQueryAvailability consoleQueryAvailability(bool armed, bool flying, bool communicationLost);

private:
    static QJsonObject _baseEvidence(const QString &state, const QString &source, const QJsonObject &observations = QJsonObject());
    static QJsonObject _unknownEvidence(const QString &source, const QString &reason);
    static QJsonObject _unavailableEvidence(const QString &source, const QString &reason);
    static QJsonObject _externalVisionInputEvidence(const QString &output, bool outputTruncated);
    static QJsonObject _externalVisionFusionEvidence(const QString &output, bool outputTruncated);
    static QJsonObject _externalVisionConfigurationEvidence(const PX4ExternalVisionSnapshot &snapshot, const QString &source);
    static QJsonObject _externalVisionConfigurationEvidenceFromConsole(const QString &output, bool outputTruncated);
};
