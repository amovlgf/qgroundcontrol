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

class Vehicle;

/// Firmware boundary for AI diagnostic context and policy. The first phase
/// intentionally registers only the PX4 provider.
class AIDiagnosticProvider
{
public:
    virtual ~AIDiagnosticProvider() = default;

    virtual QString id() const = 0;
    virtual bool supportsVehicle(const Vehicle *vehicle) const = 0;
    virtual QString unsupportedReason(const Vehicle *vehicle) const = 0;
    virtual QJsonObject diagnosticEvidence(Vehicle *vehicle) const = 0;
    virtual QString systemPromptRules() const = 0;
};
