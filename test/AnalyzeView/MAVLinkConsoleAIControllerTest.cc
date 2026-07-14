/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "MAVLinkConsoleAIControllerTest.h"
#include "PX4DiagnosticEvidence.h"

#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtTest/QTest>

namespace {

QJsonObject externalVision(const QJsonObject &evidence)
{
    return evidence.value(QStringLiteral("externalVision")).toObject();
}

}

void MAVLinkConsoleAIControllerTest::_aiConsoleSettingsPageLoads_test()
{
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/qml"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/QGroundControl/AppSettings/AIConsoleSettings.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    QScopedPointer<QObject> page(component.create());
    QVERIFY2(page, qPrintable(component.errorString()));
}

void MAVLinkConsoleAIControllerTest::_externalVisionInputPresentButNotConfigured_test()
{
    PX4ExternalVisionSnapshot snapshot;
    snapshot.sysStatusAvailable = true;
    snapshot.sysStatusPresent = true;
    snapshot.parameterManagerReady = true;
    snapshot.evCtrlAvailable = true;
    snapshot.evCtrl = 0;

    const QJsonObject snapshotEvidence = PX4DiagnosticEvidence::externalVisionSnapshot(snapshot);
    const QJsonObject configuration = externalVision(snapshotEvidence).value(QStringLiteral("configuration")).toObject();
    QCOMPARE(configuration.value(QStringLiteral("state")).toString(), QStringLiteral("confirmed"));
    const QJsonObject configurationObservations = configuration.value(QStringLiteral("observations")).toObject();
    QCOMPARE(configurationObservations.value(QStringLiteral("ekf2EvCtrl")).toInt(), 0);
    QCOMPARE(configurationObservations.value(QStringLiteral("horizontalPositionFusionEnabled")).toBool(), false);

    const QString output = QStringLiteral(
        "TOPIC: vehicle_visual_odometry\n"
        " vehicle_visual_odometry\n"
        "\ttimestamp: 209174792 (0.082695 seconds ago)\n"
        "\tposition: [-0.00160, -0.00052, 0.00079]\n"
        "\tq: [0.99999, -0.00236, -0.00163, -0.00040]\n"
        "\tvelocity: [nan, nan, nan]\n");
    const QJsonObject consoleEvidence = PX4DiagnosticEvidence::externalVisionConsoleEvidence(
        PX4DiagnosticEvidence::externalVisionConsoleCommands().at(0), output, false);
    const QJsonObject input = externalVision(consoleEvidence).value(QStringLiteral("input")).toObject();
    QCOMPARE(input.value(QStringLiteral("state")).toString(), QStringLiteral("confirmed"));
    const QJsonObject inputObservations = input.value(QStringLiteral("observations")).toObject();
    QCOMPARE(inputObservations.value(QStringLiteral("positionState")).toString(), QStringLiteral("confirmed"));
    QCOMPARE(inputObservations.value(QStringLiteral("quaternionState")).toString(), QStringLiteral("confirmed"));
    QCOMPARE(inputObservations.value(QStringLiteral("velocityProvided")).toBool(), true);
    QCOMPARE(inputObservations.value(QStringLiteral("velocityState")).toString(), QStringLiteral("invalid"));
    QVERIFY(!inputObservations.contains(QStringLiteral("rateHz")));
}

void MAVLinkConsoleAIControllerTest::_externalVisionFusionControl_test()
{
    const QString output = QStringLiteral(
        "TOPIC: estimator_status_flags\n"
        " estimator_status_flags\n"
        "\tcs_ev_pos: true\n"
        "\tcs_ev_vel: false\n"
        "\tcs_ev_yaw: true\n"
        "\tcs_ev_hgt: false\n");
    const QJsonObject evidence = PX4DiagnosticEvidence::externalVisionConsoleEvidence(
        PX4DiagnosticEvidence::externalVisionConsoleCommands().at(1), output, false);
    const QJsonObject fusion = externalVision(evidence).value(QStringLiteral("fusionControl")).toObject();
    QCOMPARE(fusion.value(QStringLiteral("state")).toString(), QStringLiteral("confirmed"));
    const QJsonObject observations = fusion.value(QStringLiteral("observations")).toObject();
    QCOMPARE(observations.value(QStringLiteral("cs_ev_pos")).toBool(), true);
    QCOMPARE(observations.value(QStringLiteral("cs_ev_vel")).toBool(), false);
    QCOMPARE(observations.value(QStringLiteral("cs_ev_yaw")).toBool(), true);
    QCOMPARE(observations.value(QStringLiteral("cs_ev_hgt")).toBool(), false);
    QVERIFY(observations.value(QStringLiteral("semanticBoundary")).toString().contains(QStringLiteral("do not")));
}

void MAVLinkConsoleAIControllerTest::_externalVisionInvalidPayload_test()
{
    const QString output = QStringLiteral(
        "TOPIC: vehicle_visual_odometry\n"
        "\ttimestamp: 209174792 (0.082695 seconds ago)\n"
        "\tposition: [nan, nan, nan]\n"
        "\tq: [1.0, 0.0, 0.0, 0.0]\n");
    const QJsonObject evidence = PX4DiagnosticEvidence::externalVisionConsoleEvidence(
        PX4DiagnosticEvidence::externalVisionConsoleCommands().at(0), output, false);
    const QJsonObject input = externalVision(evidence).value(QStringLiteral("input")).toObject();
    QCOMPARE(input.value(QStringLiteral("state")).toString(), QStringLiteral("invalid"));
    QCOMPARE(input.value(QStringLiteral("observations")).toObject().value(QStringLiteral("positionState")).toString(), QStringLiteral("invalid"));
}

void MAVLinkConsoleAIControllerTest::_externalVisionUnavailableAndUnknown_test()
{
    const QJsonObject unavailableEvidence = PX4DiagnosticEvidence::externalVisionConsoleEvidence(
        PX4DiagnosticEvidence::externalVisionConsoleCommands().at(0),
        QStringLiteral("nsh: listener: topic vehicle_visual_odometry not found\n"), false);
    QCOMPARE(externalVision(unavailableEvidence).value(QStringLiteral("input")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("unavailable"));

    const QJsonObject unknownEvidence = PX4DiagnosticEvidence::externalVisionConsoleEvidence(
        PX4DiagnosticEvidence::externalVisionConsoleCommands().at(1),
        QStringLiteral("TOPIC: estimator_status_flags\n"), false);
    QCOMPARE(externalVision(unknownEvidence).value(QStringLiteral("fusionControl")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));

    const QJsonObject timedOutEvidence = PX4DiagnosticEvidence::externalVisionConsoleEvidence(
        PX4DiagnosticEvidence::externalVisionConsoleCommands().at(0), QString(), false);
    QCOMPARE(externalVision(timedOutEvidence).value(QStringLiteral("input")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));

    const QJsonObject rejectedEvidence = PX4DiagnosticEvidence::externalVisionUnavailableEvidence(
        PX4DiagnosticEvidence::externalVisionConsoleCommands().at(1), QStringLiteral("Vehicle communication is currently lost."));
    QCOMPARE(externalVision(rejectedEvidence).value(QStringLiteral("fusionControl")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("unavailable"));

    PX4ExternalVisionSnapshot snapshot;
    const QJsonObject snapshotEvidence = PX4DiagnosticEvidence::externalVisionSnapshot(snapshot);
    QCOMPARE(externalVision(snapshotEvidence).value(QStringLiteral("configuration")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("unavailable"));
}

void MAVLinkConsoleAIControllerTest::_externalVisionRoutingAndSafety_test()
{
    QVERIFY(PX4DiagnosticEvidence::isExternalVisionQuestion(QStringLiteral("有外部视觉 VIO 数据吗？")));
    QVERIFY(PX4DiagnosticEvidence::isExternalVisionQuestion(QStringLiteral("Check vehicle_visual_odometry")));
    QVERIFY(!PX4DiagnosticEvidence::isExternalVisionQuestion(QStringLiteral("GPS 是否锁定？")));

    const QStringList commands = PX4DiagnosticEvidence::externalVisionConsoleCommands();
    QCOMPARE(commands.size(), 3);
    QCOMPARE(commands.at(0), QStringLiteral("listener vehicle_visual_odometry 1"));
    QCOMPARE(commands.at(1), QStringLiteral("listener estimator_status_flags 1"));
    QCOMPARE(commands.at(2), QStringLiteral("param show EKF2_EV_CTRL"));

    QCOMPARE(static_cast<int>(PX4DiagnosticEvidence::consoleQueryAvailability(true, false, false)), static_cast<int>(PX4DiagnosticEvidence::ConsoleQueryAvailability::ArmedOrFlying));
    QCOMPARE(static_cast<int>(PX4DiagnosticEvidence::consoleQueryAvailability(false, true, false)), static_cast<int>(PX4DiagnosticEvidence::ConsoleQueryAvailability::ArmedOrFlying));
    QCOMPARE(static_cast<int>(PX4DiagnosticEvidence::consoleQueryAvailability(false, false, true)), static_cast<int>(PX4DiagnosticEvidence::ConsoleQueryAvailability::CommunicationLost));
    QCOMPARE(static_cast<int>(PX4DiagnosticEvidence::consoleQueryAvailability(false, false, false)), static_cast<int>(PX4DiagnosticEvidence::ConsoleQueryAvailability::Allowed));

    PX4ExternalVisionSnapshot snapshot;
    snapshot.sysStatusAvailable = true;
    snapshot.sysStatusEnabled = false;
    const QJsonObject snapshotEvidence = PX4DiagnosticEvidence::externalVisionSnapshot(snapshot);
    const QJsonObject sysStatus = externalVision(snapshotEvidence).value(QStringLiteral("sysStatus")).toObject();
    QVERIFY(sysStatus.value(QStringLiteral("observations")).toObject().value(QStringLiteral("semanticBoundary")).toString().contains(QStringLiteral("do not prove")));
}
