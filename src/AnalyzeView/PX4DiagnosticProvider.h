/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "AIDiagnosticProvider.h"

#include <QtCore/QList>
#include <QtCore/QStringList>

class PX4DiagnosticProvider final : public AIDiagnosticProvider
{
public:
    struct AutomaticToolRequest {
        enum class Kind {
            SensorStatus,
            Parameter
        };

        Kind kind = Kind::SensorStatus;
        QString value;
    };

    QString id() const override;
    bool supportsVehicle(const Vehicle *vehicle) const override;
    QString unsupportedReason(const Vehicle *vehicle) const override;
    QJsonObject diagnosticEvidence(Vehicle *vehicle) const override;
    QString systemPromptRules() const override;

    QString normalizedSensorType(const QString &sensorType) const;
    QString consoleCommandForSensorStatus(const QString &sensorType) const;
    bool isSafeParameterName(const QString &paramName) const;
    bool isWhitelistedConsoleCommand(const QString &command) const;
    bool isSafeMavlinkMessageId(int messageId) const;
    bool validateLowPrivilegeMavlinkTool(Vehicle *vehicle, const QString &toolName, QString *errorText) const;
    QList<AutomaticToolRequest> automaticToolsForQuestion(const QString &question, int maximumTools) const;

private:
    QString _firstParameterNameInQuestion(const QString &question) const;
    static bool _questionContainsAny(const QString &normalizedQuestion, const QStringList &needles);
};
