#include "RidManagementUITest.h"

#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include "MockLink.h"
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
                        auto* sync = findItem(_rootItem, QStringLiteral("ridSyncActivationButton"));
                        QVERIFY(sync);
                        QVERIFY(!sync->isEnabled());
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
