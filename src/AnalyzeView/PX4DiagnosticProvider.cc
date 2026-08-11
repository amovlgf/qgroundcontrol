/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "PX4DiagnosticProvider.h"

#include "Fact.h"
#include "ParameterManager.h"
#include "PX4DiagnosticEvidence.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

QString PX4DiagnosticProvider::id() const
{
    return QStringLiteral("px4");
}

bool PX4DiagnosticProvider::supportsVehicle(const Vehicle *vehicle) const
{
    return vehicle && vehicle->px4Firmware();
}

QString PX4DiagnosticProvider::unsupportedReason(const Vehicle *vehicle) const
{
    if (!vehicle) {
        return QStringLiteral("No active vehicle is connected.");
    }
    if (!vehicle->px4Firmware()) {
        return QStringLiteral("AI Flight Diagnostics currently supports PX4 vehicles only.");
    }
    return QString();
}

QJsonObject PX4DiagnosticProvider::diagnosticEvidence(Vehicle *vehicle) const
{
    PX4ExternalVisionSnapshot externalVisionSnapshot;
    if (!vehicle || !supportsVehicle(vehicle)) {
        return PX4DiagnosticEvidence::externalVisionSnapshot(externalVisionSnapshot);
    }

    const uint32_t presentBits = static_cast<uint32_t>(vehicle->sensorsPresentBits());
    const uint32_t enabledBits = static_cast<uint32_t>(vehicle->sensorsEnabledBits());
    const uint32_t healthBits = static_cast<uint32_t>(vehicle->sensorsHealthBits());
    const uint32_t visionMask = static_cast<uint32_t>(MAV_SYS_STATUS_SENSOR_VISION_POSITION);
    externalVisionSnapshot.sysStatusAvailable = (presentBits | enabledBits | healthBits) != 0;
    externalVisionSnapshot.sysStatusPresent = (presentBits & visionMask) != 0;
    externalVisionSnapshot.sysStatusEnabled = (enabledBits & visionMask) != 0;
    externalVisionSnapshot.sysStatusHealthy = (healthBits & visionMask) != 0;

    ParameterManager *parameterManager = vehicle->parameterManager();
    externalVisionSnapshot.parameterManagerReady = parameterManager && parameterManager->parametersReady();
    if (externalVisionSnapshot.parameterManagerReady) {
        constexpr int componentId = ParameterManager::defaultComponentId;
        if (parameterManager->parameterExists(componentId, QStringLiteral("EKF2_EV_CTRL"))) {
            externalVisionSnapshot.evCtrlAvailable = true;
            externalVisionSnapshot.evCtrl = parameterManager->getParameter(componentId, QStringLiteral("EKF2_EV_CTRL"))->rawValue().toInt();
        }
        if (parameterManager->parameterExists(componentId, QStringLiteral("EKF2_HGT_REF"))) {
            externalVisionSnapshot.hgtRefAvailable = true;
            externalVisionSnapshot.hgtRef = parameterManager->getParameter(componentId, QStringLiteral("EKF2_HGT_REF"))->rawValue().toInt();
        }
    }

    return PX4DiagnosticEvidence::externalVisionSnapshot(externalVisionSnapshot);
}

QString PX4DiagnosticProvider::systemPromptRules() const
{
    return QStringLiteral(
        "For PX4 diagnostics, keep input arrival, payload validity, EKF2 configuration, EKF fusion-control state, and health/navigation status separate. "
        "SYS_STATUS Computer vision position Disabled only describes that sensor-status bit and never proves vehicle_visual_odometry input is absent. "
        "GPS lock, global-position status, and ESTIMATOR_POS_HORIZ_ABS never identify whether external-vision input is present or fused. "
        "Only report a message rate when the supplied evidence contains an explicit measurement window. "
        "Only recommend PX4 parameter names directly present in the supplied context or tool results. "
        "When a read-only diagnostic query is skipped or fails, keep that evidence layer unavailable or unknown instead of claiming that data does not exist. ");
}

QString PX4DiagnosticProvider::normalizedSensorType(const QString &sensorType) const
{
    const QString normalized = sensorType.trimmed().toLower().replace(QChar::Space, QChar('_')).replace(QChar('-'), QChar('_'));
    if (normalized == QStringLiteral("imu")) return QStringLiteral("all");
    if (normalized == QStringLiteral("accelerometer")) return QStringLiteral("accel");
    if (normalized == QStringLiteral("gyroscope")) return QStringLiteral("gyro");
    if (normalized == QStringLiteral("compass") || normalized == QStringLiteral("magnetometer")) return QStringLiteral("mag");
    if (normalized == QStringLiteral("barometer")) return QStringLiteral("baro");
    if (normalized == QStringLiteral("ekf") || normalized == QStringLiteral("ekf2")) return QStringLiteral("estimator");
    if (normalized == QStringLiteral("localposition")) return QStringLiteral("local_position");
    if (normalized == QStringLiteral("globalposition")) return QStringLiteral("global_position");
    if (normalized == QStringLiteral("rangefinder") || normalized == QStringLiteral("distance")) return QStringLiteral("distance_sensor");
    if (normalized == QStringLiteral("flow")) return QStringLiteral("optical_flow");
    if (normalized == QStringLiteral("external_vision") || normalized == QStringLiteral("vision") || normalized == QStringLiteral("visual_odometry")) {
        return PX4DiagnosticEvidence::externalVisionInputSensorType();
    }
    return normalized;
}

QString PX4DiagnosticProvider::consoleCommandForSensorStatus(const QString &sensorType) const
{
    const QString normalized = normalizedSensorType(sensorType);
    if (normalized == QStringLiteral("all")) return QStringLiteral("sensors status");
    if (normalized == QStringLiteral("accel")) return QStringLiteral("listener sensor_accel 1");
    if (normalized == QStringLiteral("gyro")) return QStringLiteral("listener sensor_gyro 1");
    if (normalized == QStringLiteral("mag")) return QStringLiteral("listener sensor_mag 1");
    if (normalized == QStringLiteral("baro")) return QStringLiteral("listener sensor_baro 1");
    if (normalized == QStringLiteral("gps")) return QStringLiteral("listener sensor_gps 1");
    if (normalized == QStringLiteral("battery")) return QStringLiteral("listener battery_status 1");
    if (normalized == QStringLiteral("estimator")) return QStringLiteral("listener estimator_status 1");
    if (normalized == QStringLiteral("local_position")) return QStringLiteral("listener vehicle_local_position 1");
    if (normalized == QStringLiteral("global_position")) return QStringLiteral("listener vehicle_global_position 1");
    if (normalized == QStringLiteral("distance_sensor")) return QStringLiteral("listener distance_sensor 1");
    if (normalized == QStringLiteral("optical_flow")) return QStringLiteral("listener sensor_optical_flow 1");
    if (normalized == PX4DiagnosticEvidence::externalVisionInputSensorType()) return PX4DiagnosticEvidence::externalVisionConsoleCommands().at(0);
    if (normalized == PX4DiagnosticEvidence::externalVisionFusionSensorType()) return PX4DiagnosticEvidence::externalVisionConsoleCommands().at(1);
    if (normalized == QStringLiteral("commander")) return QStringLiteral("commander status");
    if (normalized == QStringLiteral("mavlink")) return QStringLiteral("mavlink status");
    if (normalized == QStringLiteral("version")) return QStringLiteral("ver all");
    return QString();
}

bool PX4DiagnosticProvider::isSafeParameterName(const QString &paramName) const
{
    static const QRegularExpression regex(QStringLiteral("^[A-Z][A-Z0-9_]{0,31}$"));
    return regex.match(paramName).hasMatch();
}

bool PX4DiagnosticProvider::isWhitelistedConsoleCommand(const QString &command) const
{
    static const QStringList exactCommands{
        QStringLiteral("sensors status"), QStringLiteral("listener sensor_accel 1"),
        QStringLiteral("listener sensor_gyro 1"), QStringLiteral("listener sensor_mag 1"),
        QStringLiteral("listener sensor_baro 1"), QStringLiteral("listener sensor_gps 1"),
        QStringLiteral("listener battery_status 1"), QStringLiteral("listener estimator_status 1"),
        QStringLiteral("listener vehicle_local_position 1"), QStringLiteral("listener vehicle_global_position 1"),
        QStringLiteral("listener distance_sensor 1"), QStringLiteral("listener sensor_optical_flow 1"),
        QStringLiteral("listener vehicle_visual_odometry 1"), QStringLiteral("listener estimator_status_flags 1"),
        QStringLiteral("commander status"), QStringLiteral("mavlink status"), QStringLiteral("ver all")
    };
    if (exactCommands.contains(command)) {
        return true;
    }
    static const QRegularExpression paramShowRegex(QStringLiteral("^param show [A-Z][A-Z0-9_]{0,31}$"));
    return paramShowRegex.match(command).hasMatch();
}

bool PX4DiagnosticProvider::isSafeMavlinkMessageId(int messageId) const
{
    switch (messageId) {
    case MAVLINK_MSG_ID_HEARTBEAT:
    case MAVLINK_MSG_ID_SYS_STATUS:
    case MAVLINK_MSG_ID_SYSTEM_TIME:
    case MAVLINK_MSG_ID_GPS_RAW_INT:
    case MAVLINK_MSG_ID_ATTITUDE:
    case MAVLINK_MSG_ID_LOCAL_POSITION_NED:
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
    case MAVLINK_MSG_ID_HOME_POSITION:
    case MAVLINK_MSG_ID_VFR_HUD:
    case MAVLINK_MSG_ID_EXTENDED_SYS_STATE:
    case MAVLINK_MSG_ID_BATTERY_STATUS:
    case MAVLINK_MSG_ID_AUTOPILOT_VERSION:
    case MAVLINK_MSG_ID_ESTIMATOR_STATUS:
        return true;
    default:
        return false;
    }
}

bool PX4DiagnosticProvider::validateLowPrivilegeMavlinkTool(Vehicle *vehicle, const QString &toolName, QString *errorText) const
{
    auto reject = [&](const QString &message) {
        if (errorText) *errorText = message;
        return false;
    };
    if (!vehicle) return reject(QStringLiteral("No active vehicle is connected."));
    if (!supportsVehicle(vehicle)) return reject(QStringLiteral("Low-privilege AI MAVLink tools currently support PX4 vehicles only."));
    if (vehicle->armed() || vehicle->flying()) return reject(QStringLiteral("The vehicle is armed or flying, so low-privilege AI MAVLink tools are disabled."));
    if (vehicle->vehicleLinkManager()->communicationLost()) return reject(QStringLiteral("Vehicle communication is currently lost."));
    if (toolName != QStringLiteral("request_mavlink_message") && toolName != QStringLiteral("set_message_interval")) {
        return reject(QStringLiteral("The requested low-privilege MAVLink tool is not supported."));
    }
    return true;
}

QList<PX4DiagnosticProvider::AutomaticToolRequest> PX4DiagnosticProvider::automaticToolsForQuestion(const QString &question, int maximumTools) const
{
    QList<AutomaticToolRequest> requests;
    QStringList commands;

    auto appendRequest = [&](AutomaticToolRequest::Kind kind, const QString &value) {
        if (requests.size() >= maximumTools) {
            return;
        }
        const QString command = kind == AutomaticToolRequest::Kind::Parameter
            ? QStringLiteral("param show %1").arg(value.trimmed().toUpper())
            : consoleCommandForSensorStatus(value);
        if (command.isEmpty() || commands.contains(command) || !isWhitelistedConsoleCommand(command)) {
            return;
        }
        requests.append(AutomaticToolRequest{kind, value});
        commands.append(command);
    };

    auto appendSensor = [&](const QString &sensorType) {
        appendRequest(AutomaticToolRequest::Kind::SensorStatus, sensorType);
    };
    auto appendParameter = [&](const QString &parameterName) {
        appendRequest(AutomaticToolRequest::Kind::Parameter, parameterName);
    };

    const QString normalizedQuestion = question.toLower();
    const QString parameterName = _firstParameterNameInQuestion(question);
    if (!parameterName.isEmpty() && _questionContainsAny(normalizedQuestion, {
            QStringLiteral("param"), QStringLiteral("parameter"), QStringLiteral("参数"),
            QStringLiteral("当前值"), QStringLiteral("是多少"), QStringLiteral("查询"),
            QStringLiteral("查看"), QStringLiteral("show")
        })) {
        appendParameter(parameterName);
    }

    if (_questionContainsAny(normalizedQuestion, {
            QStringLiteral("版本"), QStringLiteral("固件"), QStringLiteral("firmware"),
            QStringLiteral("version"), QStringLiteral("git hash"), QStringLiteral("build"),
            QStringLiteral("编译"), QStringLiteral("commit")
        })) {
        appendSensor(QStringLiteral("version"));
    }
    if (_questionContainsAny(normalizedQuestion, {
            QStringLiteral("mavlink"), QStringLiteral("链路"), QStringLiteral("数传"),
            QStringLiteral("通信"), QStringLiteral("丢包"), QStringLiteral("link status")
        })) {
        appendSensor(QStringLiteral("mavlink"));
    }
    if (_questionContainsAny(normalizedQuestion, {
            QStringLiteral("commander"), QStringLiteral("解锁"), QStringLiteral("arming"),
            QStringLiteral("preflight"), QStringLiteral("起飞前"), QStringLiteral("起飞检查"),
            QStringLiteral("不能起飞"), QStringLiteral("无法起飞"), QStringLiteral("不能解锁"),
            QStringLiteral("飞控状态"), QStringLiteral("能不能起飞")
        })) {
        appendSensor(QStringLiteral("commander"));
    }

    bool matchedSpecificSensor = false;
    auto appendSpecificSensor = [&](const QString &sensorType, const QStringList &needles) {
        if (_questionContainsAny(normalizedQuestion, needles)) {
            appendSensor(sensorType);
            matchedSpecificSensor = true;
        }
    };

    appendSpecificSensor(QStringLiteral("gyro"), {
        QStringLiteral("陀螺"), QStringLiteral("gyro"), QStringLiteral("gyroscope")
    });
    appendSpecificSensor(QStringLiteral("accel"), {
        QStringLiteral("加速度"), QStringLiteral("accel"), QStringLiteral("accelerometer")
    });
    appendSpecificSensor(QStringLiteral("mag"), {
        QStringLiteral("磁罗盘"), QStringLiteral("罗盘"), QStringLiteral("磁力"),
        QStringLiteral("compass"), QStringLiteral("magnetometer"), QStringLiteral("mag ")
    });
    appendSpecificSensor(QStringLiteral("baro"), {
        QStringLiteral("气压"), QStringLiteral("baro"), QStringLiteral("barometer")
    });
    appendSpecificSensor(QStringLiteral("gps"), {
        QStringLiteral("gps"), QStringLiteral("定位"), QStringLiteral("卫星"), QStringLiteral("rtk")
    });
    appendSpecificSensor(QStringLiteral("battery"), {
        QStringLiteral("电池"), QStringLiteral("电压"), QStringLiteral("低电量"),
        QStringLiteral("battery"), QStringLiteral("voltage")
    });
    appendSpecificSensor(QStringLiteral("estimator"), {
        QStringLiteral("ekf"), QStringLiteral("estimator"), QStringLiteral("估计器"),
        QStringLiteral("姿态估计")
    });
    appendSpecificSensor(QStringLiteral("local_position"), {
        QStringLiteral("local_position"), QStringLiteral("local position"), QStringLiteral("本地位置")
    });
    appendSpecificSensor(QStringLiteral("global_position"), {
        QStringLiteral("global_position"), QStringLiteral("global position"),
        QStringLiteral("全球位置"), QStringLiteral("全局位置")
    });
    appendSpecificSensor(QStringLiteral("distance_sensor"), {
        QStringLiteral("distance_sensor"), QStringLiteral("distance sensor"),
        QStringLiteral("rangefinder"), QStringLiteral("测距"), QStringLiteral("激光")
    });
    appendSpecificSensor(QStringLiteral("optical_flow"), {
        QStringLiteral("optical_flow"), QStringLiteral("optical flow"), QStringLiteral("光流")
    });

    if (PX4DiagnosticEvidence::isExternalVisionQuestion(normalizedQuestion)) {
        appendSensor(PX4DiagnosticEvidence::externalVisionInputSensorType());
        appendSensor(PX4DiagnosticEvidence::externalVisionFusionSensorType());
        appendParameter(QStringLiteral("EKF2_EV_CTRL"));
        matchedSpecificSensor = true;
    }

    if (!matchedSpecificSensor && _questionContainsAny(normalizedQuestion, {
            QStringLiteral("传感器"), QStringLiteral("sensor"), QStringLiteral("sensors"), QStringLiteral("imu")
        })) {
        appendSensor(QStringLiteral("all"));
    }

    return requests;
}

QString PX4DiagnosticProvider::_firstParameterNameInQuestion(const QString &question) const
{
    static const QRegularExpression parameterRegex(QStringLiteral("\\b[A-Za-z][A-Za-z0-9_]{2,31}\\b"));
    QRegularExpressionMatchIterator iterator = parameterRegex.globalMatch(question);
    while (iterator.hasNext()) {
        const QString candidate = iterator.next().captured(0).trimmed().toUpper();
        if (candidate.contains(QChar('_')) && isSafeParameterName(candidate)) {
            return candidate;
        }
    }
    return QString();
}

bool PX4DiagnosticProvider::_questionContainsAny(const QString &normalizedQuestion, const QStringList &needles)
{
    for (const QString &needle : needles) {
        if (normalizedQuestion.contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}
