#include "RidManagementUITest.h"

#include <functional>
#include <memory>

#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include "MockLink.h"
#include "QGCFileDialogController.h"
#include "Vehicle.h"

UT_REGISTER_TEST(RidManagementUITest, TestLabel::Integration)

void RidManagementUITest::_setupAndToolbarRoutes()
{
    runWithMockLink([] { return MockLink::startPX4MockLink(); },
                    [&](QPointer<MockLink>, Vehicle*) {
                        navigateToConfigureView();
                        if (QTest::currentTestFailed())
                            return;
                        QVERIFY(clickSidebarButton(QStringLiteral("vehicleConfig_ridButton")));
                        QTRY_VERIFY(findItem(_rootItem, QStringLiteral("ridManagementPage")));
                        verifyPanelContentVisible(QStringLiteral("RID management"));
                        QVERIFY(findItem(_rootItem, QStringLiteral("uomRegistrationPanel")));
                        for (const auto* name : {"uomVerifyButton", "uomActivateButton", "uomDeactivateButton"}) {
                            const auto* button = findItem(_rootItem, QString::fromLatin1(name));
                            QVERIFY(button);
                            QVERIFY(!button->isEnabled());
                        }
                        auto* provision = findItem(_rootItem, QStringLiteral("ridProvisionIdentityButton"));
                        QVERIFY(provision);
                        QVERIFY(!provision->isEnabled());
                        QVERIFY(!provision->isVisible());
                        // The toolbar button calls the same main-window route; exercise that route
                        // after leaving the RID page to catch loader/navigation regressions.
                        QVERIFY(clickSidebarButton(QStringLiteral("vehicleConfig_summary")));
                        QVERIFY(QMetaObject::invokeMethod(_window, "showRidManagementPage"));
                        QTRY_VERIFY(findItem(_rootItem, QStringLiteral("ridManagementPage")));
                        verifyPanelContentVisible(QStringLiteral("RID management toolbar route"));
                    });
}

void RidManagementUITest::_uomConfigurationAndCancellation()
{
    runWithMockLink(
        [] { return MockLink::startPX4MockLink(); },
        [&](QPointer<MockLink>, Vehicle*) {
            navigateToConfigureView();
            if (QTest::currentTestFailed()) {
                return;
            }
            QVERIFY(clickSidebarButton(QStringLiteral("vehicleConfig_ridButton")));
            auto* panel = findItem(_rootItem, QStringLiteral("uomRegistrationPanel"));
            QVERIFY(panel);

            QQmlComponent component(_engine);
            component.setData(R"qml(
            import QtQml
            QtObject {
                signal identityInvalidated()
                property bool customerProfile: false
                property string productModel: ""
                property string environment: "integration"
                property string producerId: "aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb"
                property string expectedSerial: "ALABB950B950U4400001"
                property string serial: expectedSerial
                property string uid: "test-rid-uid"
                property string verifyUrl: "https://uom.caac.gov.cn/test/verify"
                property string reportUrl: "https://uom.caac.gov.cn/test/report"
                property string cancelUrl: "https://uom.caac.gov.cn/test/cancel"
                property string registrationText: "Registered"
                property string deviceActivationText: "Device not activated"
                property string reportText: "No acknowledgement"
                property string lastVerifiedAt: "2026-10-27T10:00:00Z"
                property string lastReportedAt: ""
                property string statusText: "Ready"
                property string operationBlockReason: ""
                property string pendingOperation: "deactivate"
                property string operationStage: "verifying"
                property int cooldownRemainingSeconds: 0
                property bool automaticContinuation: false
                property string credentialStatus: "Session-only credentials"
                property string safetyStatus: "Read-only safety status"
                property bool pending: false
                property bool canConfigure: true
                property bool canVerify: !pending
                property bool canActivate: !pending
                property bool canDeactivate: !pending
                property bool canRetry: pending && cooldownRemainingSeconds === 0
                property int cancellationCount: 0
                property string cancellationType: ""
                property string cancellationReason: ""
                function configurationForEnvironment(environment) {
                    return { producerId: "saved-" + environment, expectedSerial: "ALABB950B950U4400002",
                             verifyUrl: "https://uom.caac.gov.cn/" + environment + "/verify",
                             reportUrl: "https://uom.caac.gov.cn/" + environment + "/report",
                             cancelUrl: "https://uom.caac.gov.cn/" + environment + "/cancel" }
                }
                function deactivateAndCancel(type, reason) {
                    cancellationType = type
                    cancellationReason = reason
                    cancellationCount++
                }
            }
        )qml",
                              QUrl());
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            std::unique_ptr<QObject> manager(component.create());
            QVERIFY(manager);
            QVERIFY(panel->setProperty("manager", QVariant::fromValue(manager.get())));
            QVERIFY(panel->setProperty("ridManager", QVariant::fromValue(manager.get())));
            QCOMPARE(findItem(_rootItem, QStringLiteral("uomRegistrationState"))->property("text").toString(),
                     QStringLiteral("Registered"));
            QCOMPARE(findItem(_rootItem, QStringLiteral("uomDeviceActivationState"))->property("text").toString(),
                     QStringLiteral("Device not activated"));
            QCOMPARE(findItem(_rootItem, QStringLiteral("uomReportState"))->property("text").toString(),
                     QStringLiteral("No acknowledgement"));

            QVERIFY(
                clickButtonScrolled(QStringLiteral("uomConfigureButton"), QStringLiteral("ridManagementFlickable")));
            QVERIFY(waitForDialog(QStringLiteral("Configure UOM connection")));
            auto* environment = findItem(_rootItem, QStringLiteral("uomEnvironmentInput"));
            auto* firstKeyInput = findItem(_rootItem, QStringLiteral("uomKeyInput"));
            QVERIFY(environment);
            QVERIFY(firstKeyInput);
            QVERIFY(firstKeyInput->setProperty("text", QStringLiteral("test-input-not-a-key")));
            QVERIFY(environment->setProperty("currentIndex", 1));
            QVERIFY(QMetaObject::invokeMethod(environment, "activated", Q_ARG(int, 1)));
            QVERIFY(verifyText(QStringLiteral("uomProducerInput"), QStringLiteral("saved-official"),
                               QStringLiteral("load selected environment")));
            QVERIFY(verifyText(QStringLiteral("uomVerifyUrlInput"),
                               QStringLiteral("https://uom.caac.gov.cn/official/verify"),
                               QStringLiteral("load official endpoint")));
            QVERIFY(firstKeyInput->property("text").toString().isEmpty());
            QVERIFY(QMetaObject::invokeMethod(manager.get(), "identityInvalidated"));
            QTRY_VERIFY_WITH_TIMEOUT(!dialogVisible(QStringLiteral("Configure UOM connection")),
                                     TestTimeout::shortMs());

            QVERIFY(manager->setProperty("pending", true));
            QVERIFY(
                clickButtonScrolled(QStringLiteral("uomConfigureButton"), QStringLiteral("ridManagementFlickable")));
            QVERIFY(waitForDialog(QStringLiteral("Configure UOM connection")));
            for (const auto* name : {"uomEnvironmentInput", "uomExpectedSerialInput"}) {
                QVERIFY(verifyEnabled(QString::fromLatin1(name), false, QStringLiteral("pending profile locked")));
            }
            for (const auto* name :
                 {"uomProducerInput", "uomVerifyUrlInput", "uomReportUrlInput", "uomCancelUrlInput"}) {
                QVERIFY(
                    verifyEnabled(QString::fromLatin1(name), true, QStringLiteral("pending profile can be repaired")));
            }
            auto* producerInput = findItem(_rootItem, QStringLiteral("uomProducerInput"));
            QVERIFY(producerInput);
            QVERIFY(producerInput->setProperty("text", QStringLiteral("corrected-producer")));
            QVERIFY(verifyEnabled(QStringLiteral("popupDialog_acceptButton"), false,
                                  QStringLiteral("profile repair needs explicit confirmation")));
            auto* repairConfirmation = findItem(_rootItem, QStringLiteral("uomProfileRepairConfirmation"));
            QVERIFY(repairConfirmation);
            QVERIFY(repairConfirmation->isVisible());
            QVERIFY(repairConfirmation->setProperty("checked", true));
            QVERIFY(verifyEnabled(QStringLiteral("popupDialog_acceptButton"), true,
                                  QStringLiteral("confirmed profile repair can be saved")));
            auto* keyInput = findItem(_rootItem, QStringLiteral("uomKeyInput"));
            QVERIFY(keyInput);
            QVERIFY(keyInput->isEnabled());
            QCOMPARE(keyInput->property("echoMode").toInt(), 2);  // TextInput.Password
            QVERIFY(keyInput->property("text").toString().isEmpty());
            QVERIFY(rejectDialog());
            QVERIFY(clickButtonScrolled(QStringLiteral("uomRetryButton"), QStringLiteral("ridManagementFlickable")));
            QVERIFY(waitForDialog(QStringLiteral("Continue deregistration")));
            QVERIFY(manager->setProperty("uid", QStringLiteral("replacement-rid-uid")));
            QVERIFY(verifyEnabled(QStringLiteral("popupDialog_acceptButton"), false,
                                  QStringLiteral("retry remains bound to the original RID")));
            QVERIFY(rejectDialog());
            QVERIFY(manager->setProperty("uid", QStringLiteral("test-rid-uid")));

            QVERIFY(manager->setProperty("pending", false));
            QVERIFY(
                clickButtonScrolled(QStringLiteral("uomDeactivateButton"), QStringLiteral("ridManagementFlickable")));
            QVERIFY(waitForDialog(QStringLiteral("Deactivate and cancel registration")));
            auto* reasonType = findItem(_rootItem, QStringLiteral("uomCancellationTypeInput"));
            auto* reason = findItem(_rootItem, QStringLiteral("uomCancellationReasonInput"));
            auto* confirmed = findItem(_rootItem, QStringLiteral("uomCancellationConfirmed"));
            QVERIFY(reasonType);
            QVERIFY(reason);
            QVERIFY(confirmed);
            QVERIFY(reasonType->setProperty("currentIndex", 1));
            QVERIFY(reason->setProperty("text", QString(67, QChar(0x4e2d))));
            QVERIFY(confirmed->setProperty("checked", true));
            QVERIFY(verifyEnabled(QStringLiteral("popupDialog_acceptButton"), false,
                                  QStringLiteral("201-byte reason rejected")));
            QVERIFY(reason->setProperty("text", QStringLiteral("Ownership transfer")));
            QVERIFY(confirmed->setProperty("checked", false));
            QVERIFY(verifyEnabled(QStringLiteral("popupDialog_acceptButton"), false,
                                  QStringLiteral("explicit confirmation required")));
            QCOMPARE(manager->property("cancellationCount").toInt(), 0);
            QVERIFY(confirmed->setProperty("checked", true));
            QVERIFY(acceptDialog());
            QCOMPARE(manager->property("cancellationCount").toInt(), 1);
            QCOMPARE(manager->property("cancellationType").toString(), QStringLiteral("3"));
            QCOMPARE(manager->property("cancellationReason").toString(), QStringLiteral("Ownership transfer"));
            for (const int index : {0, 2}) {
                QVERIFY(clickButtonScrolled(QStringLiteral("uomDeactivateButton"),
                                            QStringLiteral("ridManagementFlickable")));
                QVERIFY(waitForDialog(QStringLiteral("Deactivate and cancel registration")));
                auto* typeInput = findItem(_rootItem, QStringLiteral("uomCancellationTypeInput"));
                auto* reasonInput = findItem(_rootItem, QStringLiteral("uomCancellationReasonInput"));
                auto* confirmation = findItem(_rootItem, QStringLiteral("uomCancellationConfirmed"));
                QVERIFY(typeInput);
                QVERIFY(reasonInput);
                QVERIFY(confirmation);
                QVERIFY(typeInput->setProperty("currentIndex", index));
                QVERIFY(reasonInput->setProperty("text", QStringLiteral("Device retired")));
                QVERIFY(confirmation->setProperty("checked", true));
                QVERIFY(acceptDialog());
                QCOMPARE(manager->property("cancellationType").toString(),
                         index == 0 ? QStringLiteral("0") : QStringLiteral("4"));
            }
            QVERIFY(panel->setProperty("manager", QVariant::fromValue<QObject*>(nullptr)));
            QVERIFY(panel->setProperty("ridManager", QVariant::fromValue<QObject*>(nullptr)));
        });
}

void RidManagementUITest::_uomCustomerProfile_data()
{
    QTest::addColumn<QString>("buttonName");
    QTest::addColumn<QString>("dialogTitle");
    QTest::addColumn<bool>("pending");
    QTest::newRow("activation") << QStringLiteral("uomActivateButton") << QStringLiteral("Activate aircraft") << false;
    QTest::newRow("cancellation") << QStringLiteral("uomDeactivateButton")
                                  << QStringLiteral("Deactivate and cancel registration") << false;
    QTest::newRow("retry") << QStringLiteral("uomRetryButton") << QStringLiteral("Continue deregistration") << true;
}

void RidManagementUITest::_uomCustomerProfile()
{
    QFETCH(QString, buttonName);
    QFETCH(QString, dialogTitle);
    QFETCH(bool, pending);
    runWithMockLink(
        [] { return MockLink::startPX4MockLink(); },
        [&](QPointer<MockLink>, Vehicle*) {
            navigateToConfigureView();
            if (QTest::currentTestFailed()) {
                return;
            }
            QVERIFY(clickSidebarButton(QStringLiteral("vehicleConfig_ridButton")));
            auto* panel = findItem(_rootItem, QStringLiteral("uomRegistrationPanel"));
            QVERIFY(panel);

            QQmlComponent component(_engine);
            component.setData(R"qml(
            import QtQml
            QtObject {
                signal identityInvalidated()
                property bool customerProfile: true
                property string productModel: ""
                property string environment: "official"
                property string serial: "ALABB950B950U4400001"
                property string uid: "test-rid-uid"
                property string registrationText: "Registered"
                property string deviceActivationText: "Device not activated"
                property string reportText: "No acknowledgement"
                property string lastVerifiedAt: "2026-10-27T10:00:00Z"
                property string lastReportedAt: ""
                property string statusText: "Ready"
                property string operationBlockReason: ""
                property string pendingOperation: "deactivate"
                property string operationStage: "verifying"
                property int cooldownRemainingSeconds: 0
                property bool automaticContinuation: false
                property string credentialStatus: "Session-only credentials"
                property string safetyStatus: "Read-only safety status"
                property bool pending: false
                property bool canConfigure: true
                property bool canVerify: !pending
                property bool canActivate: !pending
                property bool canDeactivate: !pending
                property bool canRetry: pending && cooldownRemainingSeconds === 0
                property int operationCount: 0
                function activate() { operationCount++ }
                function deactivateAndCancel(type, reason) { operationCount++ }
                function retryPending() { operationCount++ }
            }
        )qml",
                              QUrl());
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            std::unique_ptr<QObject> manager(component.create());
            QVERIFY(manager);
            QVERIFY(manager->setProperty("pending", pending));
            QVERIFY(panel->setProperty("manager", QVariant::fromValue(manager.get())));
            QVERIFY(panel->setProperty("ridManager", QVariant::fromValue(manager.get())));
            for (const auto* name : {"uomConfigureButton", "uomCurrentEnvironment", "uomCredentialStatus"}) {
                const auto* item = findItem(_rootItem, QString::fromLatin1(name));
                QVERIFY(item);
                QVERIFY(!item->isVisible());
            }
            const auto* configure = findItem(_rootItem, QStringLiteral("uomConfigureButton"));
            QVERIFY(configure);
            QVERIFY(!configure->isEnabled());
            QCOMPARE(findItem(_rootItem, QStringLiteral("uomConnectedSerial"))->property("text").toString(),
                     manager->property("serial").toString());

            QVERIFY(!findItem(_rootItem, QStringLiteral("uomOpenWebsiteButton")));
            QCOMPARE(findItem(_rootItem, QStringLiteral("uomProductModel"))->property("text").toString(),
                     QStringLiteral("Unknown"));
            if (pending) {
                QVERIFY(manager->setProperty("pendingOperation", QStringLiteral("activate")));
                QVERIFY(manager->setProperty("cooldownRemainingSeconds", 37));
                QVERIFY(manager->setProperty("automaticContinuation", true));
                auto* retry = findItem(_rootItem, QStringLiteral("uomRetryButton"));
                QVERIFY(retry);
                QVERIFY(!retry->isEnabled());
                QCOMPARE(retry->property("text").toString(), QStringLiteral("Waiting to continue activation"));
                auto* cooldown = findItem(_rootItem, QStringLiteral("uomCooldownStatus"));
                QVERIFY(cooldown && cooldown->isVisible());
                QVERIFY(cooldown->property("text").toString().contains(QStringLiteral("37")));
                QVERIFY(manager->setProperty("automaticContinuation", false));
                QCOMPARE(retry->property("text").toString(), QStringLiteral("Continue activation"));
                QVERIFY(!retry->isEnabled());
                QVERIFY(manager->setProperty("cooldownRemainingSeconds", 0));
                QVERIFY(retry->isEnabled());
                QVERIFY(manager->setProperty("pendingOperation", QStringLiteral("deactivate")));
            }

            QVERIFY(clickButtonScrolled(buttonName, QStringLiteral("ridManagementFlickable")));
            QVERIFY(waitForDialog(dialogTitle));
            QVERIFY(QMetaObject::invokeMethod(manager.get(), "identityInvalidated"));
            QTRY_VERIFY_WITH_TIMEOUT(!dialogVisible(dialogTitle), TestTimeout::shortMs());
            QCOMPARE(manager->property("operationCount").toInt(), 0);
            QVERIFY(panel->setProperty("manager", QVariant::fromValue<QObject*>(nullptr)));
            QVERIFY(panel->setProperty("ridManager", QVariant::fromValue<QObject*>(nullptr)));
        });
}

void RidManagementUITest::_ridDialogIdentityInvalidation_data()
{
    QTest::addColumn<QString>("buttonText");
    QTest::addColumn<QString>("dialogTitle");
    QTest::newRow("factory-identity") << QStringLiteral("Provision factory identity")
                                      << QStringLiteral("Provision factory identity");
    QTest::newRow("parameter") << QStringLiteral("Edit") << QStringLiteral("RID_RATE");
    QTest::newRow("restart") << QStringLiteral("Restart RID") << QStringLiteral("Restart RID");
    QTest::newRow("defaults") << QStringLiteral("Restore RID defaults") << QStringLiteral("Restore RID defaults");
}

void RidManagementUITest::_ridDialogIdentityInvalidation()
{
    QFETCH(QString, buttonText);
    QFETCH(QString, dialogTitle);
    runWithMockLink(
        [] { return MockLink::startPX4MockLink(); },
        [&](QPointer<MockLink>, Vehicle* vehicle) {
            navigateToConfigureView();
            if (QTest::currentTestFailed()) {
                return;
            }
            QVERIFY(clickSidebarButton(QStringLiteral("vehicleConfig_ridButton")));
            auto* page = findItem(_rootItem, QStringLiteral("ridManagementPage"));
            QVERIFY(page);
            QQmlComponent component(_engine);
            component.setData(R"qml(
            import QtQml
            QtObject {
                id: fakeVehicle
                signal identityInvalidated()
                property var ridManager: fakeVehicle
                property var uomRegistrationManager: null
                property string statusText: "Ready"
                property bool available: true
                property bool busy: false
                property var devices: []
                property int selectedDevice: -1
                property bool supported: true
                property string serial: ""
                property string uid: "test-rid-uid"
                property int nodeId: 125
                property int activationState: 0
                property bool factoryProvisionSupported: true
                property bool canProvision: true
                property bool canWrite: true
                property bool locked: false
                property QtObject parameterFact: QtObject {
                    property string name: "RID_RATE"
                    property string valueString: "1"
                }
                property var parameters: [{ fact: parameterFact, masked: false, writable: true,
                                             action: false, unused: false, rebootRequired: false }]
                property int operationCount: 0
                function factoryIdentityError(serial, aircraftClass) { return "" }
                function provisionFactoryIdentity(uid, serial, aircraftClass) { operationCount++ }
                function writeParameter(name, value) { operationCount++ }
                function reboot() { operationCount++ }
                function resetDefaults() { operationCount++ }
            }
        )qml",
                              QUrl());
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            std::unique_ptr<QObject> manager(component.create());
            QVERIFY(manager);
            QVERIFY(page->setProperty("vehicle", QVariant::fromValue(manager.get())));
            const std::function<QQuickItem*(QQuickItem*)> findAction = [&](QQuickItem* item) -> QQuickItem* {
                if (item->property("text").toString() == buttonText &&
                    item->metaObject()->indexOfSignal("clicked()") >= 0) {
                    return item;
                }
                for (auto* child : item->childItems()) {
                    if (auto* action = findAction(child)) {
                        return action;
                    }
                }
                return nullptr;
            };
            QQuickItem* action = nullptr;
            QTRY_VERIFY_WITH_TIMEOUT((action = findAction(page)), TestTimeout::shortMs());
            action->setObjectName(QStringLiteral("ridTestAction"));
            QVERIFY(clickButtonScrolled(QStringLiteral("ridTestAction"), QStringLiteral("ridManagementFlickable")));
            QVERIFY(waitForDialog(dialogTitle));
            if (buttonText == QStringLiteral("Provision factory identity")) {
                auto* aircraftClass = findItem(_rootItem, QStringLiteral("ridFactoryClassInput"));
                auto* confirmed = findItem(_rootItem, QStringLiteral("ridFactoryBindingConfirmed"));
                QVERIFY(aircraftClass && confirmed);
                QCOMPARE(aircraftClass->property("currentIndex").toInt(), -1);
                QCOMPARE(aircraftClass->property("alternateText").toString(), QStringLiteral("Select aircraft class"));
                QVERIFY(confirmed->setProperty("checked", true));
                QVERIFY(verifyEnabled(QStringLiteral("popupDialog_acceptButton"), false,
                                      QStringLiteral("aircraft class is required")));
                QVERIFY(aircraftClass->setProperty("currentIndex", 1));
                QCOMPARE(aircraftClass->property("currentText").toString(), QStringLiteral("Light"));
                QVERIFY(verifyEnabled(QStringLiteral("popupDialog_acceptButton"), true,
                                      QStringLiteral("aircraft class selected")));
            }
            QVERIFY(QMetaObject::invokeMethod(manager.get(), "identityInvalidated"));
            QTRY_VERIFY_WITH_TIMEOUT(!dialogVisible(dialogTitle), TestTimeout::shortMs());
            QCOMPARE(manager->property("operationCount").toInt(), 0);
            QVERIFY(page->setProperty("vehicle", QVariant::fromValue(vehicle)));
        });
}

void RidManagementUITest::_uomReportingStateSeparation()
{
    runWithMockLink([] { return MockLink::startPX4MockLink(); },
                    [&](QPointer<MockLink>, Vehicle*) {
                        navigateToConfigureView();
                        if (QTest::currentTestFailed()) {
                            return;
                        }
                        QVERIFY(clickSidebarButton(QStringLiteral("vehicleConfig_ridButton")));
                        auto* panel = findItem(_rootItem, QStringLiteral("uomReportingPanel"));
                        QVERIFY(panel);
                        QQmlComponent component(_engine);
                        component.setData(R"qml(
            import QtQml
            QtObject {
                property bool localReady: true
                property bool networkHealthy: false
                property bool activated: true
                property bool protocolCompatible: true
                property bool broadcastReady: true
                property string aircraftId: "ALABB950B950U4400001"
                property string reportingModel: "QGC network reporting"
                property string localStatusText: "Local state verified"
                property string networkStatusText: "Waiting for platform acknowledgement"
                property int pendingReports: 4
                property int heldReports: 2
                property double lastAckAgeMs: 4294967295
                property bool reloadCalled: false
                property url exportedPath: ""
                function reloadReportingConfiguration() { reloadCalled = true }
                function exportReportingDiagnostics(path) { exportedPath = path; return true }
            }
        )qml",
                                          QUrl());
                        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
                        std::unique_ptr<QObject> manager(component.create());
                        QVERIFY(manager);
                        QVERIFY(panel->setProperty("manager", QVariant::fromValue(manager.get())));
                        const auto labelText = [this](const char* name) {
                            const auto* item = findItem(_rootItem, QString::fromLatin1(name));
                            return item ? item->property("text").toString() : QString();
                        };
                        QCOMPARE(labelText("uomReportingLocalReady"), QStringLiteral("Ready"));
                        QCOMPARE(labelText("uomReportingNetworkHealthy"), QStringLiteral("Not confirmed"));
                        QCOMPARE(labelText("uomReportingPendingCount"), QStringLiteral("4"));
                        QCOMPARE(labelText("uomReportingHeldCount"), QStringLiteral("2"));
                        QCOMPARE(labelText("uomReportingLastAck"), QStringLiteral("None recorded"));
                        QVERIFY(manager->setProperty("networkHealthy", true));
                        QVERIFY(manager->setProperty("lastAckAgeMs", 2000));
                        QTRY_COMPARE(labelText("uomReportingNetworkHealthy"), QStringLiteral("Confirmed"));
                        QCOMPARE(labelText("uomReportingLocalReady"), QStringLiteral("Ready"));
                        QCOMPARE(labelText("uomReportingLastAck"), QStringLiteral("2 seconds ago"));
                        QVERIFY(clickButtonScrolled(QStringLiteral("uomReloadReportingConfiguration"),
                                                    QStringLiteral("ridManagementFlickable")));
                        QVERIFY(manager->property("reloadCalled").toBool());
                        QTemporaryDir directory;
                        QVERIFY(directory.isValid());
                        const QString path = directory.filePath(QStringLiteral("reporting.json"));
                        QGCFileDialogController::setTestNextFileForAccept(path);
                        QVERIFY(clickButtonScrolled(QStringLiteral("uomExportReportingDiagnostics"),
                                                    QStringLiteral("ridManagementFlickable")));
                        QTRY_COMPARE(manager->property("exportedPath").toUrl(), QUrl::fromLocalFile(path));
                    });
}
