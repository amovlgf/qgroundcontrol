#include <QtTest/QTest>

#include "ManualScheduler.h"
#include "MockUomProvider.h"
#include "UomSession.h"

class ControlledProvider final : public UomProvider
{
public:
    Uom::Environment environment() const override { return Uom::Environment::Mock; }

    void authenticate(quint64 generation, const Uom::Identity&) override
    {
        authGeneration = generation;
        ++authCount;
    }

    void submit(quint64 generation, quint64 sequence, const Uom::Identity&, const Uom::Telemetry&) override
    {
        reportGeneration = generation;
        reportSequence = sequence;
        ++reportCount;
    }

    void cancel() override { ++cancelCount; }

    quint64 authGeneration = 0, reportGeneration = 0, reportSequence = 0;
    int authCount = 0, reportCount = 0, cancelCount = 0;
};

class UomTest final : public QObject
{
    Q_OBJECT
private:
    static Uom::Identity identity()
    {
        Uom::Identity id;
        id.aircraftId = "TESTAIRCRAFT00000001";
        id.moduleId = "001122334455";
        id.revision = 1;
        id.bootId = 2;
        id.ridBootId = 3;
        id.digest = id.computedDigest();
        id.activated = true;
        return id;
    }

    static Uom::Telemetry telemetry(quint64 now)
    {
        Uom::Telemetry data;
        data.utcMilliseconds = 1800000000000;
        data.receivedUs = now;
        data.positionValid = data.operatorValid = true;
        return data;
    }

private slots:

    void identityValidation()
    {
        auto id = identity();
        QVERIFY(id.valid());
        id.registrationId.clear();
        QVERIFY(id.valid());
        id.bootId = 0;
        QVERIFY(!id.valid());
    }

    void digestMatchesContractVector()
    {
        auto id = identity();
        id.moduleId = "001122AABBCC";
        id.aircraftClass = 2;
        id.registrationId = "ABC12345678";
        id.broadcastSuffix = "12345678";
        QCOMPARE(id.computedDigest().toHex(),
                 QByteArray("66889b17d854a8d840151771f9c2e8e183a70c6e3827fc880b48d68fb2a25ba4"));
        id.digest = id.computedDigest();
        QVERIFY(id.valid());
        id.registrationId[0] = u'X';
        QVERIFY(!id.valid());
        id.digest = id.computedDigest();
        QVERIFY(id.valid());
        id.broadcastSuffix = "87654321";
        id.digest = id.computedDigest();
        QVERIFY(!id.valid());
        id.registrationId.clear();
        id.aircraftId[0] = u' ';
        id.digest = id.computedDigest();
        QVERIFY(!id.valid());
    }

    void requestDeadline()
    {
        ManualScheduler clock;
        ControlledProvider provider;
        UomSession session(&provider, nullptr, &clock);
        session.setIdentity(identity());
        session.setTelemetry(telemetry(clock.nowUs()));
        QVERIFY(clock.advanceBy(std::chrono::seconds(2)));
        session.setTelemetry(telemetry(clock.nowUs()));
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(1200)));
        QCOMPARE(session.fault(), Uom::Fault::Timeout);
        QCOMPARE(session.state(), Uom::State::Recovering);
        QVERIFY(!session.authenticated());
    }

    void officialNotConfigured()
    {
        ManualScheduler clock;
        UnconfiguredUomProvider provider;
        UomSession session(&provider, nullptr, &clock);
        session.setIdentity(identity());
        session.setTelemetry(telemetry(clock.nowUs()));
        QVERIFY(clock.advanceBy(std::chrono::seconds(1)));
        QCOMPARE(session.state(), Uom::State::NotConfigured);
        QVERIFY(!session.officialReady());
        QVERIFY(!session.simulationReady());
    }

    void sessionIsolationAndStaleCallbacks()
    {
        ManualScheduler clock;
        ControlledProvider first, second;
        UomSession a(&first, nullptr, &clock), b(&second, nullptr, &clock);
        a.setIdentity(identity());
        b.setIdentity(identity());
        a.setTelemetry(telemetry(clock.nowUs()));
        b.setTelemetry(telemetry(clock.nowUs()));
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(100)));
        emit first.authenticated(first.authGeneration, true, Uom::Fault::None, {});
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(100)));
        emit first.acknowledged(first.reportGeneration, first.reportSequence, true, Uom::Fault::None, {});
        QVERIFY(a.simulationReady());
        QVERIFY(!a.officialReady());
        QVERIFY(!b.simulationReady());
        const auto oldGeneration = first.reportGeneration;
        const auto oldSequence = first.reportSequence;
        auto changed = identity();
        ++changed.ridBootId;
        a.setIdentity(changed);
        emit first.acknowledged(oldGeneration, oldSequence, true, Uom::Fault::None, {});
        QVERIFY(!a.simulationReady());
        QVERIFY(!a.authenticated());
    }

    void activationAndFreshness()
    {
        ManualScheduler clock;
        ControlledProvider provider;
        UomSession session(&provider, nullptr, &clock);
        auto id = identity();
        id.activated = false;
        session.setIdentity(id);
        session.setTelemetry(telemetry(clock.nowUs()));
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(100)));
        QCOMPARE(provider.authCount, 0);
        QCOMPARE(session.fault(), Uom::Fault::Activation);
        id.activated = true;
        session.setIdentity(id);
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(100)));
        emit provider.authenticated(provider.authGeneration, true, Uom::Fault::None, {});
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(100)));
        emit provider.acknowledged(provider.reportGeneration, provider.reportSequence, true, Uom::Fault::None, {});
        QVERIFY(session.simulationReady());
        QVERIFY(clock.advanceBy(std::chrono::seconds(3)));
        QVERIFY(!session.simulationReady());
        QCOMPARE(session.fault(), Uom::Fault::Telemetry);
    }

    void retryAndTimeout()
    {
        ManualScheduler clock;
        ControlledProvider provider;
        UomSession session(&provider, nullptr, &clock);
        session.setIdentity(identity());
        session.setTelemetry(telemetry(clock.nowUs()));
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(100)));
        emit provider.authenticated(provider.authGeneration, false, Uom::Fault::Authentication, "rejected");
        QCOMPARE(session.state(), Uom::State::Recovering);
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(900)));
        QCOMPARE(provider.authCount, 1);
        session.setTelemetry(telemetry(clock.nowUs()));
        QVERIFY(clock.advanceBy(std::chrono::milliseconds(100)));
        QCOMPARE(provider.authCount, 2);
        const auto old = provider.authGeneration;
        session.reset();
        emit provider.authenticated(old, true, Uom::Fault::None, {});
        QVERIFY(!session.authenticated());
    }

    void localhostOnly()
    {
        QVERIFY(MockUomProvider::isLocalEndpoint(QUrl("http://127.0.0.1:8080")));
        QVERIFY(!MockUomProvider::isLocalEndpoint(QUrl("https://uom.example.com")));
        QVERIFY(!MockUomProvider::isLocalEndpoint(QUrl("http://user:pass@localhost")));
    }
};
QTEST_GUILESS_MAIN(UomTest)
#include "UomTest.moc"
