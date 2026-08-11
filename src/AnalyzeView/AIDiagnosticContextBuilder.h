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
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtPositioning/QGeoCoordinate>

class Fact;
class FactGroup;
class Vehicle;

/// Builds the versioned, read-only QGroundControl context supplied to the AI
/// diagnostic service. Firmware-specific evidence is delegated to the active
/// diagnostic provider rather than being inferred by the chat controller.
class AIDiagnosticContextBuilder
{
public:
    static QJsonObject buildContext(Vehicle *vehicle,
                                    const QJsonObject &px4DiagnosticEvidence,
                                    const QString &consoleAttachment,
                                    bool consoleAttachmentTruncated);
    static QJsonObject buildVehicleSnapshot(Vehicle *vehicle, const QJsonObject &px4DiagnosticEvidence);
    static QJsonObject buildHealthAndArmingCheckReport(Vehicle *vehicle);
    static QJsonObject buildLinkStatus(Vehicle *vehicle);
    static QJsonObject factGroupToJson(const FactGroup *factGroup);
    static QJsonObject parameterToJson(const Fact *fact);

private:
    static QJsonObject _coordinateToJson(const QGeoCoordinate &coordinate);
    static QJsonObject _buildSysStatusSensorInfo(Vehicle *vehicle);
    static QJsonObject _factToJson(const Fact *fact);
    static QJsonValue _variantToJson(const QVariant &value);
};
