#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtSql/QSqlQuery>
#include <QtTest/QTest>

#include "UomReportStore.h"

using namespace UomReporting;

namespace {
Capture capture()
{
    Capture value;
    value.profile = {.mode = Mode::Isolated,
                     .endpoint = QUrl(QStringLiteral("http://127.0.0.1:12345/ridData")),
                     .appId = QStringLiteral("offline-app"),
                     .uasModel = QStringLiteral("TEST-MODEL"),
                     .uscCode = QStringLiteral("TEST-USC"),
                     .platform = QStringLiteral("QGC-test"),
                     .credentialReference = QStringLiteral("offline-key")};
    value.sample.aircraftId = QStringLiteral("1581F45T0123456789AB");
    value.sample.timestampMs =
        QDateTime::fromString(QStringLiteral("2026-11-04T15:59:59Z"), Qt::ISODate).toMSecsSinceEpoch();
    value.sample.operationState = 2;
    value.identity = {{"uid", "001122AABBCC"}, {"revision", 1}};
    value.bootId = 123;
    value.flightSequence = 1;
    value.takeoffUtcMs = value.sample.timestampMs - 1000;
    value.connection = 5;
    value.airborne = value.valid = true;
    return value;
}

bool executeSql(const QString& path, const QString& sql)
{
    const auto name = QUuid::createUuid().toString();
    bool success = false;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            success = query.exec(sql);
            query.finish();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return success;
}
}  // namespace

class UomReportStoreTest : public QObject
{
    Q_OBJECT
private slots:

    void immutableFlightAndMidnightRecovery()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("reports.sqlite"));
        auto item = capture();
        QString orderId;
        QByteArray original;
        {
            UomReportStore store(path);
            QVERIFY(store.ready());
            QVERIFY(store.append(item, item.sample.timestampMs).value_or(0) > 0);
            const auto pending = store.pending(item.sample.timestampMs);
            QCOMPARE(pending.size(), 1);
            orderId = pending[0].orderId;
            original = pending[0].body;
            QVERIFY(orderId.startsWith(QStringLiteral("20261104-")));
            QCOMPARE(orderId.size(), 17);
            QCOMPARE(original, encode(item.sample)->toHex().toUpper());
            QVERIFY(buildRequest(pending[0].profile, QStringLiteral("test-key"), orderId, original));
            QCOMPARE(store.append(item, item.sample.timestampMs), std::optional<qint64>(0));
            QCOMPARE(store.stats().total, 1);
        }
        UomReportStore store(path);
        QVERIFY(store.ready());
        item.sample.timestampMs += 2000;  // Across midnight in Asia/Shanghai, in the same flight.
        item.connection += 1;
        QVERIFY(store.append(item, item.sample.timestampMs).value_or(0) > 0);
        const auto pending = store.pending(item.sample.timestampMs);
        QCOMPARE(pending.size(), 2);
        for (const auto& delivery : pending) {
            QCOMPARE(delivery.orderId, orderId);
            QCOMPARE(delivery.profile.uasModel, item.profile.uasModel);
            QCOMPARE(delivery.identity, item.identity);
        }
        bool originalPreserved = false;
        for (const auto& delivery : pending) {
            originalPreserved |= delivery.body == original;
        }
        QVERIFY(originalPreserved);
        item.flightSequence += 1;
        item.takeoffUtcMs = item.sample.timestampMs;
        item.sample.timestampMs += 1000;
        QVERIFY(store.append(item, item.sample.timestampMs).value_or(0) > 0);
        const auto nextFlight = store.pending(item.sample.timestampMs);
        bool foundNewFlight = false;
        for (const auto& delivery : nextFlight) {
            if (delivery.flightSequence == item.flightSequence) {
                foundNewFlight = true;
                QVERIFY(delivery.orderId != orderId);
                QVERIFY(delivery.orderId.startsWith(QStringLiteral("20261105-")));
            }
        }
        QVERIFY(foundNewFlight);
    }

    void groundAndIdentityChangesAreHeld_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto* name : {"ground", "identity", "model", "manufacturer", "takeoff", "invalid"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void groundAndIdentityChangesAreHeld()
    {
        QFETCH(QString, change);
        UomReportStore store(QStringLiteral(":memory:"));
        auto item = capture();
        QVERIFY(store.append(item, item.sample.timestampMs));
        item.sample.timestampMs += 1000;
        if (change == QStringLiteral("ground")) {
            item.airborne = false;
        } else if (change == QStringLiteral("identity")) {
            item.identity["revision"] = 2;
        } else if (change == QStringLiteral("model")) {
            item.profile.uasModel = QStringLiteral("OTHER-MODEL");
        } else if (change == QStringLiteral("manufacturer")) {
            item.profile.uscCode = QStringLiteral("OTHER-MANUFACTURER");
        } else if (change == QStringLiteral("takeoff")) {
            ++item.takeoffUtcMs;
        } else {
            item.valid = false;
        }
        QVERIFY(store.append(item, item.sample.timestampMs));
        QCOMPARE(store.stats().held, 1);
        QCOMPARE(store.pending(item.sample.timestampMs).size(), 1);
    }

    void crashRecoveryPreservesBodyAndAttempt()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("reports.sqlite"));
        const auto item = capture();
        Delivery original;
        {
            UomReportStore store(path);
            QVERIFY(store.append(item, item.sample.timestampMs));
            const auto pending = store.pending(item.sample.timestampMs);
            QCOMPARE(pending.size(), 1);
            original = pending.first();
            QVERIFY(store.reserve(original.id, item.sample.timestampMs));
            QVERIFY(!store.reserve(original.id, item.sample.timestampMs));
            QVERIFY(store.pending(item.sample.timestampMs).isEmpty());
        }
        UomReportStore store(path);
        const auto due = store.pending(item.sample.timestampMs);
        QCOMPARE(due.size(), 1);
        QCOMPARE(due[0].attempts, 1);
        QCOMPARE(due[0].body, original.body);
        QCOMPARE(due[0].orderId, original.orderId);
        QCOMPARE(due[0].sampledMs, original.sampledMs);
        QVERIFY(store.reserve(original.id, item.sample.timestampMs));
        QVERIFY(store.finish(original.id, {.success = true, .httpStatus = 200, .code = 200}, 0));
        QCOMPARE(store.stats().accepted, 1);
        QVERIFY(!store.finish(original.id, {.retryable = true}, 0));
        QCOMPARE(store.stats().accepted, 1);
    }

    void fairDueSelectionAndRetryDeadline()
    {
        UomReportStore store(QStringLiteral(":memory:"));
        auto first = capture();
        const qint64 now = first.sample.timestampMs + 200000;
        for (int i = 0; i < 140; ++i) {
            first.sample.timestampMs += 1000;
            QVERIFY(store.append(first, now));
        }
        auto second = capture();
        second.sample.aircraftId = QStringLiteral("2581F45T0123456789AB");
        QVERIFY(store.append(second, now));
        auto due = store.pending(now, 2);
        QCOMPARE(due.size(), 2);
        QVERIFY(due[0].aircraftId != due[1].aircraftId);
        const auto id = due[0].id;
        QVERIFY(store.reserve(id, now));
        QVERIFY(store.finish(id, {.retryable = true, .httpStatus = 500}, now + 1000));
        for (const auto& delivery : store.pending(now, 200)) {
            QVERIFY(delivery.id != id);
        }
        bool found = false;
        for (const auto& delivery : store.pending(now + 1000, 200)) {
            found |= delivery.id == id && delivery.attempts == 1;
        }
        QVERIFY(found);
    }

    void newestStoredSampleWinsDespiteGnssClockOffset()
    {
        UomReportStore store(QStringLiteral(":memory:"));
        auto item = capture();
        const qint64 now = item.sample.timestampMs - 3600000;
        qint64 latestId = 0;
        for (int i = 0; i < 140; ++i) {
            item.sample.timestampMs += 1000;
            const auto id = store.append(item, now - 2000 + i * 10);
            QVERIFY(id);
            latestId = *id;
        }
        const auto due = store.pending(now, 1);
        QCOMPARE(due.size(), 1);
        QCOMPARE(due[0].id, latestId);
        QVERIFY(store.reserve(latestId, now));
        QVERIFY(store.finish(latestId, {.retryable = true}, now + 30000));
        bool recoverable = false;
        for (const auto& delivery : store.pending(now - 60000, 200)) {
            recoverable |= delivery.id == latestId;
        }
        QVERIFY(recoverable);
    }

    void manufacturerFilterPrecedesCandidateLimit_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto* name : {"environment", "account", "endpoint", "model", "manufacturer", "platform"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void manufacturerFilterPrecedesCandidateLimit()
    {
        QFETCH(QString, change);
        UomReportStore store(QStringLiteral(":memory:"));
        auto previous = capture();
        if (change == QStringLiteral("environment")) {
            previous.profile.mode = Mode::Commissioning;
            previous.profile.endpoint = QUrl(QStringLiteral("https://uom.receive.caacic.cn/ridData"));
        }
        constexpr int backlogSize = 140;
        for (int i = 0; i < backlogSize; ++i) {
            QVERIFY(store.append(previous, previous.sample.timestampMs).value_or(0) > 0);
            previous.sample.timestampMs += 500;
        }
        auto current = previous;
        ++current.flightSequence;
        current.takeoffUtcMs = current.sample.timestampMs - 1000;
        if (change == QStringLiteral("environment")) {
            current.profile.mode = Mode::Official;
        } else if (change == QStringLiteral("account")) {
            current.profile.appId = QStringLiteral("new-account");
        } else if (change == QStringLiteral("endpoint")) {
            current.profile.endpoint = QUrl(QStringLiteral("http://127.0.0.1:12346/ridData"));
        } else if (change == QStringLiteral("model")) {
            current.profile.uasModel = QStringLiteral("NEW-MODEL");
        } else if (change == QStringLiteral("manufacturer")) {
            current.profile.uscCode = QStringLiteral("NEW-MANUFACTURER");
        } else {
            current.profile.platform = QStringLiteral("new-platform");
        }
        const auto currentId = store.append(current, current.sample.timestampMs);
        QVERIFY(currentId.value_or(0) > 0);
        auto authorized = current.profile;
        authorized.credentialReference = QStringLiteral("rotated-key");
        authorized.backfillValidated = true;
        const auto due =
            store.pending(current.sample.timestampMs + 2000, 128, {{current.sample.aircraftId, authorized}});
        QCOMPARE(due.size(), 1);
        QCOMPARE(due[0].id, *currentId);
        QCOMPARE(due[0].body, encode(current.sample)->toHex().toUpper());
        QCOMPARE(due[0].profile.credentialReference, current.profile.credentialReference);
        QCOMPARE(store.stats().pending, backlogSize + 1);
        QCOMPARE(store.stats().total, backlogSize + 1);
    }

    void deliveryTimesDoNotRewriteSourceTimestamp()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("reports.sqlite"));
        auto capture = ::capture();
        const qint64 storedMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 sentMs = storedMs + 1;
        QByteArray frozenBody;
        {
            UomReportStore store(path);
            QVERIFY(store.append(capture, storedMs));
            const auto pending = store.pending(storedMs);
            QCOMPARE(pending.size(), 1);
            frozenBody = pending[0].body;
            QVERIFY(store.reserve(pending[0].id, sentMs));
            QVERIFY(store.finish(pending[0].id, {.success = true, .httpStatus = 200, .code = 200}, sentMs + 1000));
            QVERIFY(store.eventSummary().value("lastConfirmedUtcMs").toString().toLongLong() >= storedMs);
        }
        const QString name = QStringLiteral("delivery-times-check");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(
                query.exec(QStringLiteral("SELECT source_ms,samples.stored_ms,sent_ms,completed_ms,confirmed_ms,body "
                                          "FROM samples JOIN delivery ON sample_id=id")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toULongLong(), capture.sample.timestampMs);
            QCOMPARE(query.value(1).toLongLong(), storedMs);
            QCOMPARE(query.value(2).toLongLong(), sentMs);
            QVERIFY(query.value(3).toLongLong() >= storedMs);
            QCOMPARE(query.value(4).toLongLong(), query.value(3).toLongLong());
            QCOMPARE(query.value(5).toByteArray(), frozenBody);
            query.finish();
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
    }

    void eventsSurviveUnchangedSourceTimestamp()
    {
        UomReportStore store(QStringLiteral(":memory:"));
        auto item = capture();
        QVERIFY(store.append(item, item.sample.timestampMs));
        item.valid = false;
        QCOMPARE(store.append(item, item.sample.timestampMs + 1000), std::optional<qint64>(0));
        QVERIFY(
            store.recordEvent(QStringLiteral("invalid-data"), item.sample.aircraftId, item.sample.timestampMs + 1000));
        QVERIFY(
            store.recordEvent(QStringLiteral("disconnected"), item.sample.aircraftId, item.sample.timestampMs + 2000));
        QVERIFY(store.recordEvent(QStringLiteral("authentication"), item.sample.aircraftId,
                                  item.sample.timestampMs + 3000, 200, 401));
        QCOMPARE(store.stats().total, 1);
        const auto summary = store.eventSummary();
        QCOMPARE(summary.value("invalid-data").toInt(), 1);
        QCOMPARE(summary.value("disconnected").toInt(), 1);
        QCOMPARE(summary.value("lastFailureKind").toString(), QStringLiteral("authentication"));
        QCOMPARE(summary.value("lastCode").toInt(), 401);
        QVERIFY(!QJsonDocument(summary).toJson().contains(item.sample.aircraftId.toUtf8()));
    }

    void storageFailureClosesReadiness()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("reports.sqlite"));
        UomReportStore store(path);
        QVERIFY(store.ready());
        QVERIFY(store.append(capture(), capture().sample.timestampMs));
        QVERIFY(executeSql(path, QStringLiteral("DROP TABLE delivery")));
        QVERIFY(store.pending(capture().sample.timestampMs).isEmpty());
        QVERIFY(!store.ready());
        QVERIFY(!store.error().isEmpty());
        QVERIFY(!store.append(capture(), capture().sample.timestampMs));
        const QString corruptPath = directory.filePath(QStringLiteral("corrupt.sqlite"));
        QFile corrupt(corruptPath);
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("not a sqlite database");
        corrupt.close();
        UomReportStore corruptStore(corruptPath);
        QVERIFY(!corruptStore.ready());
    }
};

QTEST_GUILESS_MAIN(UomReportStoreTest)
#include "UomReportStoreTest.moc"
