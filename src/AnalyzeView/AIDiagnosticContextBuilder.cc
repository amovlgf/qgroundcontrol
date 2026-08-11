/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "AIDiagnosticContextBuilder.h"

#include "Fact.h"
#include "FactGroup.h"
#include "HealthAndArmingCheckReport.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QMetaType>
#include <QtCore/QStringList>

#include <cmath>

QJsonObject AIDiagnosticContextBuilder::buildContext(Vehicle *vehicle,
                                                     const QJsonObject &px4DiagnosticEvidence,
                                                     const QString &consoleAttachment,
                                                     bool consoleAttachmentTruncated)
{
    QJsonObject context = buildVehicleSnapshot(vehicle, px4DiagnosticEvidence);
    QJsonObject attachment{
        { QStringLiteral("available"), !consoleAttachment.isEmpty() },
        { QStringLiteral("source"), QStringLiteral("User-provided PX4 console evidence") },
        { QStringLiteral("truncated"), consoleAttachmentTruncated }
    };
    if (!consoleAttachment.isEmpty()) {
        attachment.insert(QStringLiteral("text"), consoleAttachment);
    }
    context.insert(QStringLiteral("consoleAttachment"), attachment);
    return context;
}

QJsonObject AIDiagnosticContextBuilder::buildVehicleSnapshot(Vehicle *vehicle, const QJsonObject &px4DiagnosticEvidence)
{
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("schemaVersion"), 1);
    snapshot.insert(QStringLiteral("source"), QStringLiteral("QGroundControl AI Flight Diagnostics"));
    snapshot.insert(QStringLiteral("timestampUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    snapshot.insert(QStringLiteral("activeVehicle"), vehicle != nullptr);

    QJsonObject firmware{
        { QStringLiteral("available"), vehicle != nullptr },
        { QStringLiteral("supported"), vehicle && vehicle->px4Firmware() }
    };
    if (vehicle) {
        firmware.insert(QStringLiteral("type"), vehicle->firmwareTypeString());
        firmware.insert(QStringLiteral("typeId"), static_cast<int>(vehicle->firmwareType()));
    }
    snapshot.insert(QStringLiteral("firmware"), firmware);

    if (!vehicle) {
        snapshot.insert(QStringLiteral("note"), QStringLiteral("No active vehicle is connected. Only explicitly attached PX4 console evidence may be available."));
        snapshot.insert(QStringLiteral("core"), QJsonObject{ { QStringLiteral("available"), false } });
        snapshot.insert(QStringLiteral("sysStatusSensorInfo"), QJsonObject{ { QStringLiteral("available"), false } });
        snapshot.insert(QStringLiteral("healthAndArmingCheckReport"), QJsonObject{ { QStringLiteral("available"), false } });
        snapshot.insert(QStringLiteral("linkStatus"), QJsonObject{ { QStringLiteral("available"), false } });
        snapshot.insert(QStringLiteral("px4DiagnosticEvidence"), px4DiagnosticEvidence);
        snapshot.insert(QStringLiteral("factGroups"), QJsonObject());
        snapshot.insert(QStringLiteral("batteries"), QJsonArray());
        return snapshot;
    }

    QJsonObject core;
    core.insert(QStringLiteral("available"), true);
    core.insert(QStringLiteral("vehicleId"), vehicle->id());
    core.insert(QStringLiteral("firmware"), vehicle->firmwareTypeString());
    core.insert(QStringLiteral("vehicleType"), vehicle->vehicleTypeString());
    core.insert(QStringLiteral("armed"), vehicle->armed());
    core.insert(QStringLiteral("flying"), vehicle->flying());
    core.insert(QStringLiteral("landing"), vehicle->landing());
    core.insert(QStringLiteral("flightMode"), vehicle->flightMode());
    core.insert(QStringLiteral("readyToFlyAvailable"), vehicle->readyToFlyAvailable());
    core.insert(QStringLiteral("readyToFly"), vehicle->readyToFly());
    core.insert(QStringLiteral("prearmError"), vehicle->prearmError());
    core.insert(QStringLiteral("allSensorsHealthy"), vehicle->allSensorsHealthy());
    core.insert(QStringLiteral("requiresGpsFix"), vehicle->requiresGpsFix());
    core.insert(QStringLiteral("communicationLost"), vehicle->vehicleLinkManager()->communicationLost());
    core.insert(QStringLiteral("mavlinkSentCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkSentCount())));
    core.insert(QStringLiteral("mavlinkReceivedCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkReceivedCount())));
    core.insert(QStringLiteral("mavlinkLossCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkLossCount())));
    core.insert(QStringLiteral("mavlinkLossPercent"), vehicle->mavlinkLossPercent());
    core.insert(QStringLiteral("coordinate"), _coordinateToJson(vehicle->coordinate()));
    core.insert(QStringLiteral("homePosition"), _coordinateToJson(vehicle->homePosition()));
    snapshot.insert(QStringLiteral("core"), core);
    snapshot.insert(QStringLiteral("sysStatusSensorInfo"), _buildSysStatusSensorInfo(vehicle));
    snapshot.insert(QStringLiteral("healthAndArmingCheckReport"), buildHealthAndArmingCheckReport(vehicle));
    snapshot.insert(QStringLiteral("linkStatus"), buildLinkStatus(vehicle));
    snapshot.insert(QStringLiteral("px4DiagnosticEvidence"), px4DiagnosticEvidence);

    QJsonObject factGroups;
    factGroups.insert(QStringLiteral("vehicle"), factGroupToJson(vehicle));
    for (const QString &groupName : vehicle->factGroupNames()) {
        factGroups.insert(groupName, factGroupToJson(vehicle->getFactGroup(groupName)));
    }
    snapshot.insert(QStringLiteral("factGroups"), factGroups);

    QJsonArray batteries;
    QmlObjectListModel *batteryModel = vehicle->batteries();
    for (int i = 0; i < batteryModel->count(); ++i) {
        if (const FactGroup *batteryGroup = qobject_cast<const FactGroup*>(batteryModel->get(i))) {
            batteries.append(factGroupToJson(batteryGroup));
        }
    }
    snapshot.insert(QStringLiteral("batteries"), batteries);

    return snapshot;
}

QJsonObject AIDiagnosticContextBuilder::_coordinateToJson(const QGeoCoordinate &coordinate)
{
    QJsonObject object;
    object.insert(QStringLiteral("valid"), coordinate.isValid());
    if (coordinate.isValid()) {
        object.insert(QStringLiteral("latitude"), coordinate.latitude());
        object.insert(QStringLiteral("longitude"), coordinate.longitude());
        if (!qIsNaN(coordinate.altitude())) {
            object.insert(QStringLiteral("altitude"), coordinate.altitude());
        }
    }
    return object;
}

QJsonObject AIDiagnosticContextBuilder::_buildSysStatusSensorInfo(Vehicle *vehicle)
{
    QJsonObject object;
    if (!vehicle || !vehicle->sysStatusSensorInfo()) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    QObject *sensorInfo = vehicle->sysStatusSensorInfo();
    const QStringList sensorNames = sensorInfo->property("sensorNames").toStringList();
    const QStringList sensorStatus = sensorInfo->property("sensorStatus").toStringList();
    QJsonArray sensors;
    const int count = qMin(sensorNames.size(), sensorStatus.size());
    for (int i = 0; i < count; ++i) {
        sensors.append(QJsonObject{
            { QStringLiteral("name"), sensorNames.at(i) },
            { QStringLiteral("status"), sensorStatus.at(i) }
        });
    }

    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("sensors"), sensors);
    return object;
}

QJsonObject AIDiagnosticContextBuilder::buildHealthAndArmingCheckReport(Vehicle *vehicle)
{
    QJsonObject object;
    if (!vehicle || !vehicle->healthAndArmingCheckReport()) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    HealthAndArmingCheckReport *report = vehicle->healthAndArmingCheckReport();
    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("supported"), report->supported());
    object.insert(QStringLiteral("canArm"), report->canArm());
    object.insert(QStringLiteral("canTakeoff"), report->canTakeoff());
    object.insert(QStringLiteral("canStartMission"), report->canStartMission());
    object.insert(QStringLiteral("hasWarningsOrErrors"), report->hasWarningsOrErrors());
    object.insert(QStringLiteral("gpsState"), report->gpsState());

    QJsonArray problems;
    if (QmlObjectListModel *problemModel = report->problemsForCurrentMode()) {
        for (int i = 0; i < problemModel->count(); ++i) {
            const QObject *problem = problemModel->get(i);
            if (problem) {
                problems.append(QJsonObject{
                    { QStringLiteral("message"), problem->property("message").toString() },
                    { QStringLiteral("description"), problem->property("description").toString() },
                    { QStringLiteral("severity"), problem->property("severity").toString() }
                });
            }
        }
    }
    object.insert(QStringLiteral("problemsForCurrentMode"), problems);
    return object;
}

QJsonObject AIDiagnosticContextBuilder::buildLinkStatus(Vehicle *vehicle)
{
    QJsonObject object;
    if (!vehicle) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("communicationLost"), vehicle->vehicleLinkManager()->communicationLost());
    object.insert(QStringLiteral("mavlinkSentCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkSentCount())));
    object.insert(QStringLiteral("mavlinkReceivedCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkReceivedCount())));
    object.insert(QStringLiteral("mavlinkLossCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkLossCount())));
    object.insert(QStringLiteral("mavlinkLossPercent"), vehicle->mavlinkLossPercent());
    object.insert(QStringLiteral("messagesReceived"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->messagesReceived())));
    object.insert(QStringLiteral("messagesSent"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->messagesSent())));
    object.insert(QStringLiteral("messagesLost"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->messagesLost())));
    object.insert(QStringLiteral("telemetryRRSSI"), vehicle->telemetryRRSSI());
    object.insert(QStringLiteral("telemetryLRSSI"), vehicle->telemetryLRSSI());
    object.insert(QStringLiteral("telemetryRXErrors"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->telemetryRXErrors())));
    object.insert(QStringLiteral("telemetryFixed"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->telemetryFixed())));
    object.insert(QStringLiteral("telemetryTXBuffer"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->telemetryTXBuffer())));
    object.insert(QStringLiteral("telemetryLNoise"), vehicle->telemetryLNoise());
    object.insert(QStringLiteral("telemetryRNoise"), vehicle->telemetryRNoise());
    object.insert(QStringLiteral("mavlinkSigning"), vehicle->mavlinkSigning());
    return object;
}

QJsonObject AIDiagnosticContextBuilder::factGroupToJson(const FactGroup *factGroup)
{
    QJsonObject object;
    if (!factGroup) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("telemetryAvailable"), factGroup->telemetryAvailable());
    QJsonObject facts;
    for (const QString &factName : factGroup->factNames()) {
        facts.insert(factName, _factToJson(factGroup->getFact(factName)));
    }
    object.insert(QStringLiteral("facts"), facts);
    return object;
}

QJsonObject AIDiagnosticContextBuilder::_factToJson(const Fact *fact)
{
    QJsonObject object;
    if (!fact) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("name"), fact->name());
    object.insert(QStringLiteral("componentId"), fact->componentId());
    object.insert(QStringLiteral("value"), _variantToJson(fact->cookedValue()));
    object.insert(QStringLiteral("rawValue"), _variantToJson(fact->rawValue()));
    object.insert(QStringLiteral("valueString"), fact->cookedValueString());
    object.insert(QStringLiteral("rawValueString"), fact->rawValueString());
    object.insert(QStringLiteral("rawValueStringFullPrecision"), fact->rawValueStringFullPrecision());
    object.insert(QStringLiteral("units"), fact->cookedUnits());
    object.insert(QStringLiteral("type"), static_cast<int>(fact->type()));
    object.insert(QStringLiteral("shortDescription"), fact->shortDescription());
    object.insert(QStringLiteral("longDescription"), fact->longDescription());
    object.insert(QStringLiteral("category"), fact->category());
    object.insert(QStringLiteral("group"), fact->group());
    object.insert(QStringLiteral("decimalPlaces"), fact->decimalPlaces());
    object.insert(QStringLiteral("defaultValueAvailable"), fact->defaultValueAvailable());
    if (fact->defaultValueAvailable()) {
        object.insert(QStringLiteral("defaultValue"), _variantToJson(fact->cookedDefaultValue()));
        object.insert(QStringLiteral("defaultValueString"), fact->cookedDefaultValueString());
        object.insert(QStringLiteral("valueEqualsDefault"), fact->valueEqualsDefault());
    }
    object.insert(QStringLiteral("min"), _variantToJson(fact->cookedMin()));
    object.insert(QStringLiteral("minString"), fact->cookedMinString());
    object.insert(QStringLiteral("minIsDefaultForType"), fact->minIsDefaultForType());
    object.insert(QStringLiteral("max"), _variantToJson(fact->cookedMax()));
    object.insert(QStringLiteral("maxString"), fact->cookedMaxString());
    object.insert(QStringLiteral("maxIsDefaultForType"), fact->maxIsDefaultForType());
    object.insert(QStringLiteral("increment"), fact->cookedIncrement());
    object.insert(QStringLiteral("readOnly"), fact->readOnly());
    object.insert(QStringLiteral("writeOnly"), fact->writeOnly());
    object.insert(QStringLiteral("volatileValue"), fact->volatileValue());
    object.insert(QStringLiteral("vehicleRebootRequired"), fact->vehicleRebootRequired());
    object.insert(QStringLiteral("qgcRebootRequired"), fact->qgcRebootRequired());
    object.insert(QStringLiteral("enumStrings"), QJsonArray::fromStringList(fact->enumStrings()));
    object.insert(QStringLiteral("enumValues"), QJsonArray::fromVariantList(fact->enumValues()));
    object.insert(QStringLiteral("bitmaskStrings"), QJsonArray::fromStringList(fact->bitmaskStrings()));
    object.insert(QStringLiteral("bitmaskValues"), QJsonArray::fromVariantList(fact->bitmaskValues()));
    object.insert(QStringLiteral("selectedBitmaskStrings"), QJsonArray::fromStringList(fact->selectedBitmaskStrings()));
    return object;
}

QJsonObject AIDiagnosticContextBuilder::parameterToJson(const Fact *fact)
{
    QJsonObject object = _factToJson(fact);
    object.insert(QStringLiteral("kind"), QStringLiteral("px4Parameter"));
    if (fact) {
        object.insert(QStringLiteral("parameterName"), fact->name());
        object.insert(QStringLiteral("componentId"), fact->componentId());
    }
    return object;
}

QJsonValue AIDiagnosticContextBuilder::_variantToJson(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) {
        return QJsonValue();
    }

    switch (value.metaType().id()) {
    case QMetaType::Float:
    case QMetaType::Double: {
        const double number = value.toDouble();
        return std::isfinite(number) ? QJsonValue(number) : QJsonValue();
    }
    default:
        return QJsonValue::fromVariant(value);
    }
}
