#include "UomReportStore.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QStorageInfo>
#include <QtCore/QTimeZone>
#include <QtCore/QUuid>
#include <QtSql/QSqlQuery>

namespace {
QString json(const QJsonObject& object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString accountScope(const UomReporting::Profile& profile)
{
    return QString::number(static_cast<int>(profile.mode)) + QLatin1Char('/') + profile.appId;
}
}  // namespace

UomReportStore::UomReportStore(const QString& path)
    : _connection(QStringLiteral("UomReports-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
    , _path(path)
{
    if (path.isEmpty()) {
        _fail();
        return;
    }
    if (path != QStringLiteral(":memory:")) {
        const QString directory = QFileInfo(path).absolutePath();
        if (!QDir().mkpath(directory)) {
            _fail();
            return;
        }
    }
    _database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), _connection);
    _database.setDatabaseName(path);
    _database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=1000"));
    if (!_database.open() || (path != QStringLiteral(":memory:") &&
                              !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner))) {
        _fail();
        return;
    }
    _ready = _initialize();
}

UomReportStore::~UomReportStore()
{
    _database.close();
    _database = {};
    QSqlDatabase::removeDatabase(_connection);
}

bool UomReportStore::_fail()
{
    if (_ready && _database.isOpen()) {
        QSqlQuery event(_database);
        event.prepare(
            QStringLiteral("INSERT INTO events(received_ms,aircraft,kind,http_status,code,count) "
                           "VALUES(?,'','storage-error',0,0,1)"));
        event.addBindValue(QDateTime::currentMSecsSinceEpoch());
        (void) event.exec();
    }
    _ready = false;
    _error = QStringLiteral("UOM reporting storage is unavailable; reliable caching has stopped");
    return false;
}

bool UomReportStore::_initialize()
{
    QSqlQuery query(_database);
    if (!query.exec(QStringLiteral("PRAGMA quick_check")) || !query.next() ||
        query.value(0).toString() != QStringLiteral("ok") || !query.exec(QStringLiteral("PRAGMA user_version")) ||
        !query.next() || query.value(0).toInt() > 3) {
        return _fail();
    }
    const int version = query.value(0).toInt();
    const QStringList statements{
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("PRAGMA synchronous=FULL"),
        QStringLiteral("PRAGMA foreign_keys=ON"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS flights (scope TEXT NOT NULL, aircraft TEXT NOT NULL, "
                       "boot TEXT NOT NULL, sequence INTEGER NOT NULL, takeoff_ms INTEGER NOT NULL, "
                       "identity TEXT NOT NULL, model TEXT NOT NULL, order_id TEXT NOT NULL UNIQUE, "
                       "PRIMARY KEY(scope,aircraft,boot,sequence))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS samples (id INTEGER PRIMARY KEY, scope TEXT NOT NULL, "
                       "aircraft TEXT NOT NULL, boot TEXT NOT NULL, flight_sequence INTEGER NOT NULL, "
                       "source_ms INTEGER NOT NULL, stored_ms INTEGER NOT NULL, identity TEXT NOT NULL, "
                       "profile TEXT NOT NULL, sample TEXT NOT NULL, order_id TEXT NOT NULL, body BLOB NOT NULL, "
                       "issue TEXT NOT NULL, UNIQUE(scope,aircraft,boot,flight_sequence,source_ms))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS delivery (sample_id INTEGER PRIMARY KEY REFERENCES samples(id), "
                       "state TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, next_ms INTEGER NOT NULL DEFAULT 0, "
                       "http_status INTEGER NOT NULL DEFAULT 0, code INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS due_delivery ON delivery(state,next_ms,sample_id)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY, received_ms INTEGER NOT NULL, "
                       "aircraft TEXT NOT NULL, kind TEXT NOT NULL, http_status INTEGER NOT NULL, "
                       "code INTEGER NOT NULL, count INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS sample_order ON samples(order_id,id)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS delivery_totals (aircraft TEXT NOT NULL,state TEXT NOT NULL, "
                       "sample_count INTEGER NOT NULL CHECK(sample_count>=0),PRIMARY KEY(aircraft,state))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS event_totals (kind TEXT PRIMARY KEY,total INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS last_failure_event ON events(id DESC) "
                       "WHERE kind NOT IN ('accepted','data-recovered')")};
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            return _fail();
        }
    }
    if (!_database.transaction()) {
        return _fail();
    }
    if (version < 2) {
        const QStringList migration{
            QStringLiteral("ALTER TABLE delivery ADD COLUMN aircraft TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE delivery ADD COLUMN stored_ms INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("UPDATE delivery SET aircraft=(SELECT aircraft FROM samples WHERE id=sample_id), "
                           "stored_ms=(SELECT stored_ms FROM samples WHERE id=sample_id)"),
            QStringLiteral(
                "INSERT INTO delivery_totals SELECT aircraft,state,COUNT(*) FROM delivery GROUP BY aircraft,state"),
            QStringLiteral("INSERT INTO event_totals SELECT kind,SUM(count) FROM events GROUP BY kind")};
        for (const QString& statement : migration) {
            if (!query.exec(statement)) {
                _database.rollback();
                return _fail();
            }
        }
    }
    if (version < 3) {
        const QStringList timingMigration{
            QStringLiteral("ALTER TABLE delivery ADD COLUMN sent_ms INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE delivery ADD COLUMN completed_ms INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE delivery ADD COLUMN confirmed_ms INTEGER NOT NULL DEFAULT 0")};
        for (const QString& statement : timingMigration) {
            if (!query.exec(statement)) {
                _database.rollback();
                return _fail();
            }
        }
    }
    const QStringList maintenance{
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS last_delivery_completion ON delivery(completed_ms DESC) WHERE completed_ms>0"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS active_aircraft_history ON delivery(aircraft,sample_id) "
                       "WHERE state IN ('queued','retry','uncertain')"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS active_aircraft_latest ON delivery(aircraft,stored_ms DESC,sample_id) "
            "WHERE state='queued' AND attempts=0"),
        QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS delivery_insert AFTER INSERT ON delivery BEGIN "
            "UPDATE delivery SET aircraft=(SELECT aircraft FROM samples WHERE id=NEW.sample_id), "
            "stored_ms=(SELECT stored_ms FROM samples WHERE id=NEW.sample_id) WHERE sample_id=NEW.sample_id; "
            "INSERT INTO delivery_totals SELECT aircraft,state,1 FROM delivery WHERE sample_id=NEW.sample_id "
            "ON CONFLICT(aircraft,state) DO UPDATE SET sample_count=sample_count+1; END"),
        QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS delivery_transition AFTER UPDATE OF state ON delivery "
            "WHEN OLD.state!=NEW.state BEGIN "
            "UPDATE delivery_totals SET sample_count=sample_count-1 WHERE aircraft=OLD.aircraft AND state=OLD.state; "
            "INSERT INTO delivery_totals VALUES(NEW.aircraft,NEW.state,1) "
            "ON CONFLICT(aircraft,state) DO UPDATE SET sample_count=sample_count+1; END"),
        QStringLiteral("CREATE TRIGGER IF NOT EXISTS delivery_delete AFTER DELETE ON delivery BEGIN "
                       "UPDATE delivery_totals SET sample_count=sample_count-1 WHERE aircraft=OLD.aircraft AND "
                       "state=OLD.state; END"),
        QStringLiteral("CREATE TRIGGER IF NOT EXISTS event_insert AFTER INSERT ON events BEGIN "
                       "INSERT INTO event_totals VALUES(NEW.kind,NEW.count) "
                       "ON CONFLICT(kind) DO UPDATE SET total=total+NEW.count; END"),
        QStringLiteral("UPDATE delivery SET state='uncertain' WHERE state='sending'"),
        QStringLiteral("PRAGMA user_version=3")};
    for (const QString& statement : maintenance) {
        if (!query.exec(statement)) {
            _database.rollback();
            return _fail();
        }
    }
    if (!_database.commit()) {
        _database.rollback();
        return _fail();
    }
    return true;
}

std::optional<QString> UomReportStore::_orderId(const UomReporting::Capture& capture)
{
    if (!capture.flightSequence || capture.takeoffUtcMs < 1577836800000ULL || !capture.bootId) {
        return QString{};
    }
    QSqlQuery query(_database);
    query.prepare(
        QStringLiteral("SELECT takeoff_ms,identity,model,order_id, "
                       "(SELECT profile FROM samples WHERE order_id=flights.order_id ORDER BY id LIMIT 1) "
                       "FROM flights "
                       "WHERE scope=? AND aircraft=? AND boot=? AND sequence=?"));
    query.addBindValue(accountScope(capture.profile));
    query.addBindValue(capture.sample.aircraftId);
    query.addBindValue(QString::number(capture.bootId));
    query.addBindValue(capture.flightSequence);
    if (!query.exec()) {
        _fail();
        return std::nullopt;
    }
    const QString identity = json(capture.identity);
    if (query.next()) {
        if (query.value(0).toULongLong() != capture.takeoffUtcMs || query.value(1).toString() != identity ||
            query.value(2).toString() != capture.profile.uasModel) {
            return QString{};
        }
        const auto previous =
            UomReporting::Profile::fromJson(QJsonDocument::fromJson(query.value(4).toString().toUtf8()).object());
        if (!previous || previous->endpoint != capture.profile.endpoint ||
            previous->uscCode != capture.profile.uscCode || previous->platform != capture.profile.platform ||
            previous->source != capture.profile.source) {
            return QString{};
        }
        return query.value(3).toString();
    }
    if (!capture.airborne) {
        return QString{};
    }
    // UOM flight dates use China Standard Time; the fixed offset also works without OS tzdata.
    const QDateTime takeoff =
        QDateTime::fromMSecsSinceEpoch(capture.takeoffUtcMs, QTimeZone::fromSecondsAheadOfUtc(8 * 3600));
    if (!takeoff.isValid() || capture.takeoffUtcMs > capture.sample.timestampMs) {
        return QString{};
    }
    static constexpr char ALPHABET[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    QString orderId = takeoff.toString(QStringLiteral("yyyyMMdd")) + QLatin1Char('-');
    for (int i = 0; i < 8; ++i) {
        orderId += QLatin1Char(ALPHABET[QRandomGenerator::global()->bounded(62U)]);
    }
    query.prepare(QStringLiteral("INSERT INTO flights VALUES(?,?,?,?,?,?,?,?)"));
    query.addBindValue(accountScope(capture.profile));
    query.addBindValue(capture.sample.aircraftId);
    query.addBindValue(QString::number(capture.bootId));
    query.addBindValue(capture.flightSequence);
    query.addBindValue(qint64(capture.takeoffUtcMs));
    query.addBindValue(identity);
    query.addBindValue(capture.profile.uasModel);
    query.addBindValue(orderId);
    if (!query.exec()) {
        _fail();
        return std::nullopt;
    }
    return orderId;
}

std::optional<qint64> UomReportStore::append(const UomReporting::Capture& capture, qint64 nowMs)
{
    if (!_ready) {
        return std::nullopt;
    }
    if (_path != QStringLiteral(":memory:")) {
        const QStorageInfo storage(QFileInfo(_path).absolutePath());
        if (storage.isValid() && storage.bytesAvailable() < 128LL * 1024 * 1024) {
            _fail();
            return std::nullopt;
        }
    }
    if (!_database.transaction()) {
        _fail();
        return std::nullopt;
    }
    const auto orderId = _orderId(capture);
    if (!orderId) {
        _database.rollback();
        return std::nullopt;
    }
    QString issue = capture.issue;
    QByteArray body("");
    if (capture.valid) {
        const auto encoded = UomReporting::encode(capture.sample, &issue);
        if (encoded) {
            body = encoded->toHex().toUpper();
        }
    }
    QSqlQuery query(_database);
    query.prepare(
        QStringLiteral("INSERT OR IGNORE INTO samples(scope,aircraft,boot,flight_sequence,source_ms,"
                       "stored_ms,identity,profile,sample,order_id,body,issue) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(accountScope(capture.profile));
    query.addBindValue(capture.sample.aircraftId);
    query.addBindValue(QString::number(capture.bootId));
    query.addBindValue(capture.flightSequence);
    query.addBindValue(qint64(capture.sample.timestampMs));
    query.addBindValue(nowMs);
    query.addBindValue(json(capture.identity));
    query.addBindValue(json(capture.profile.toJson()));
    QJsonObject sample = capture.sample.toJson();
    auto provenance = capture.provenance;
    provenance.insert(QStringLiteral("connectionSession"), QString::number(capture.connection));
    sample.insert(QStringLiteral("sourceMetadata"), provenance);
    query.addBindValue(json(sample));
    query.addBindValue(orderId->isEmpty() ? QStringLiteral("") : *orderId);
    query.addBindValue(body);
    query.addBindValue(issue.isEmpty() ? QStringLiteral("") : issue);
    if (!query.exec()) {
        _database.rollback();
        _fail();
        return std::nullopt;
    }
    if (!query.numRowsAffected()) {
        _database.rollback();
        return qint64(0);
    }
    const qint64 id = query.lastInsertId().toLongLong();
    // Ground records await verified platform semantics and never receive a placeholder order ID.
    const bool canSend = capture.valid && capture.airborne && !orderId->isEmpty() && !body.isEmpty();
    query.prepare(QStringLiteral("INSERT INTO delivery(sample_id,state) VALUES(?,?)"));
    query.addBindValue(id);
    query.addBindValue(canSend ? QStringLiteral("queued") : QStringLiteral("held"));
    if (!query.exec() || !_database.commit()) {
        _database.rollback();
        _fail();
        return std::nullopt;
    }
    return id;
}

QList<UomReporting::Delivery> UomReportStore::pending(qint64 nowMs, int limit,
                                                      const QHash<QString, UomReporting::Profile>& profiles)
{
    QList<UomReporting::Delivery> result;
    if (!_ready) {
        return result;
    }
    limit = qBound(1, limit, 4096);
    QSqlQuery aircraft(_database);
    if (!aircraft.exec(
            QStringLiteral("SELECT DISTINCT aircraft FROM delivery_totals "
                           "WHERE state IN ('queued','retry','uncertain') AND sample_count>0 ORDER BY aircraft"))) {
        _fail();
        return result;
    }
    QStringList eligibleAircraft;
    while (aircraft.next()) {
        const QString id = aircraft.value(0).toString();
        if (profiles.isEmpty() || profiles.contains(id)) {
            eligibleAircraft.append(id);
        }
    }
    const int perAircraft = qMax(1, (limit + int(eligibleAircraft.size()) - 1) / qMax(1, int(eligibleAircraft.size())));
    QList<QList<UomReporting::Delivery>> recent;
    QList<QList<UomReporting::Delivery>> history;
    for (const QString& aircraftId : eligibleAircraft) {
        const auto authorizedProfile = profiles.constFind(aircraftId);
        // Exclude previous manufacturer configurations before they can occupy the candidate limit.
        const QString profileSelection =
            authorizedProfile == profiles.cend()
                ? QString()
                : QStringLiteral(
                      "AND s.scope=? AND json_extract(s.profile,'$.endpoint')=? "
                      "AND json_extract(s.profile,'$.uasModel')=? AND json_extract(s.profile,'$.uscCode')=? "
                      "AND json_extract(s.profile,'$.platform')=? AND json_extract(s.profile,'$.source')=? ");
        for (bool live : {true, false}) {
            QSqlQuery query(_database);
            const QString selection =
                live ? QStringLiteral("d.state='queued' AND d.attempts=0 AND d.stored_ms BETWEEN ? AND ? ")
                     : QStringLiteral(
                           "d.state IN ('queued','retry','uncertain') "
                           "AND NOT(d.state='queued' AND d.attempts=0 AND d.stored_ms BETWEEN ? AND ?) ");
            query.prepare(
                QStringLiteral(
                    "SELECT s.id,s.profile,s.aircraft,s.order_id,s.body,s.source_ms,d.attempts,"
                    "s.boot,s.flight_sequence,s.identity,s.sample FROM delivery d JOIN samples s ON s.id=d.sample_id "
                    "WHERE d.aircraft=? AND ") +
                selection + QStringLiteral("AND (d.next_ms<=? OR d.next_ms>?) ") + profileSelection +
                QStringLiteral("ORDER BY ") +
                (live ? QStringLiteral("d.stored_ms DESC,d.sample_id") : QStringLiteral("d.sample_id")) +
                QStringLiteral(" LIMIT ?"));
            query.addBindValue(aircraftId);
            query.addBindValue(nowMs - 1000);
            query.addBindValue(nowMs + 1000);
            query.addBindValue(nowMs);
            query.addBindValue(nowMs + 30000);
            if (authorizedProfile != profiles.cend()) {
                query.addBindValue(accountScope(*authorizedProfile));
                query.addBindValue(authorizedProfile->endpoint.toString(QUrl::FullyEncoded));
                query.addBindValue(authorizedProfile->uasModel);
                query.addBindValue(authorizedProfile->uscCode);
                query.addBindValue(authorizedProfile->platform);
                query.addBindValue(authorizedProfile->source);
            }
            query.addBindValue(perAircraft);
            if (!query.exec()) {
                _fail();
                return {};
            }
            QList<UomReporting::Delivery> candidates;
            while (query.next()) {
                const auto profile = UomReporting::Profile::fromJson(
                    QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object());
                if (profile) {
                    candidates.append({query.value(0).toLongLong(), *profile, query.value(2).toString(),
                                       query.value(3).toString(), query.value(4).toByteArray(),
                                       query.value(5).toLongLong(), query.value(6).toInt(),
                                       query.value(7).toULongLong(), query.value(8).toUInt(),
                                       QJsonDocument::fromJson(query.value(9).toString().toUtf8()).object(),
                                       QJsonDocument::fromJson(query.value(10).toString().toUtf8())
                                           .object()
                                           .value(QStringLiteral("sourceMetadata"))
                                           .toObject()
                                           .value(QStringLiteral("connectionSession"))
                                           .toString()
                                           .toULongLong()});
                }
            }
            (live ? recent : history).append(std::move(candidates));
        }
    }
    // Select only bounded index ranges, then interleave aircraft without sorting their full backlogs.
    for (const auto& phase : {recent, history}) {
        for (qsizetype offset = 0; result.size() < limit; ++offset) {
            bool found = false;
            for (const auto& candidates : phase) {
                if (offset < candidates.size()) {
                    result.append(candidates[offset]);
                    found = true;
                    if (result.size() == limit) {
                        break;
                    }
                }
            }
            if (!found) {
                break;
            }
        }
    }
    return result;
}

bool UomReportStore::reserve(qint64 id, qint64 nowMs)
{
    QSqlQuery query(_database);
    query.prepare(
        QStringLiteral("UPDATE delivery SET state='sending',attempts=attempts+1,next_ms=?,sent_ms=? "
                       "WHERE sample_id=? AND state IN ('queued','retry','uncertain')"));
    query.addBindValue(nowMs);
    query.addBindValue(nowMs);
    query.addBindValue(id);
    if (!_ready || !query.exec()) {
        return _fail();
    }
    return query.numRowsAffected() == 1;
}

bool UomReportStore::finish(qint64 id, const UomReporting::Result& result, qint64 nextAttemptMs)
{
    QString state = QStringLiteral("rejected");
    if (result.success) {
        state = QStringLiteral("accepted");
    } else if (result.retryable || result.authenticationFailure) {
        state = QStringLiteral("retry");
    }
    QSqlQuery query(_database);
    query.prepare(
        QStringLiteral("UPDATE delivery SET state=?,next_ms=?,http_status=?,code=?,completed_ms=?,confirmed_ms=? "
                       "WHERE sample_id=? AND state='sending'"));
    query.addBindValue(state);
    query.addBindValue(nextAttemptMs);
    query.addBindValue(result.httpStatus);
    query.addBindValue(result.code);
    const qint64 completedMs = QDateTime::currentMSecsSinceEpoch();
    query.addBindValue(completedMs);
    query.addBindValue(result.success ? completedMs : 0);
    query.addBindValue(id);
    if (!_ready || !query.exec()) {
        return _fail();
    }
    return query.numRowsAffected() == 1;
}

QStringList UomReportStore::aircraftIds()
{
    QStringList aircraft;
    QSqlQuery query(_database);
    if (!_ready || !query.exec(QStringLiteral(
                       "SELECT DISTINCT aircraft FROM delivery_totals WHERE sample_count>0 ORDER BY aircraft"))) {
        _fail();
        return aircraft;
    }
    while (query.next()) {
        aircraft.append(query.value(0).toString());
    }
    return aircraft;
}

UomReporting::QueueStats UomReportStore::stats(const QString& aircraftId)
{
    UomReporting::QueueStats stats;
    QSqlQuery query(_database);
    if (aircraftId.isEmpty()) {
        query.prepare(QStringLiteral("SELECT state,SUM(sample_count) FROM delivery_totals GROUP BY state"));
    } else {
        query.prepare(QStringLiteral("SELECT state,sample_count FROM delivery_totals WHERE aircraft=?"));
        query.addBindValue(aircraftId);
    }
    if (!_ready || !query.exec()) {
        _fail();
        return stats;
    }
    while (query.next()) {
        const QString state = query.value(0).toString();
        const qint64 count = query.value(1).toLongLong();
        stats.total += count;
        if (state == QStringLiteral("accepted")) {
            stats.accepted += count;
        } else if (state == QStringLiteral("rejected")) {
            stats.rejected += count;
        } else if (state == QStringLiteral("held")) {
            stats.held += count;
        } else {
            stats.pending += count;
        }
    }
    return stats;
}

bool UomReportStore::checkpoint()
{
    QSqlQuery query(_database);
    if (!_ready || !query.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL)"))) {
        return _fail();
    }
    return true;
}

bool UomReportStore::recordEvent(const QString& kind, const QString& aircraftId, qint64 receivedMs, int httpStatus,
                                 int code, qint64 count)
{
    static const QStringList kinds{QStringLiteral("invalid-data"),   QStringLiteral("data-recovered"),
                                   QStringLiteral("disconnected"),   QStringLiteral("capture-gap"),
                                   QStringLiteral("accepted"),       QStringLiteral("retryable"),
                                   QStringLiteral("authentication"), QStringLiteral("rejected")};
    if (!_ready || !kinds.contains(kind) || receivedMs <= 0 || count <= 0) {
        return false;
    }
    QSqlQuery query(_database);
    query.prepare(
        QStringLiteral("INSERT INTO events(received_ms,aircraft,kind,http_status,code,count) VALUES(?,?,?,?,?,?)"));
    query.addBindValue(receivedMs);
    query.addBindValue(aircraftId.isEmpty() ? QStringLiteral("") : aircraftId);
    query.addBindValue(kind);
    query.addBindValue(httpStatus);
    query.addBindValue(code);
    query.addBindValue(count);
    return query.exec() || _fail();
}

QJsonObject UomReportStore::eventSummary()
{
    QJsonObject summary;
    if (!_database.isOpen()) {
        return summary;
    }
    QSqlQuery query(_database);
    if (!query.exec(QStringLiteral("SELECT kind,total FROM event_totals"))) {
        return summary;
    }
    while (query.next()) {
        summary.insert(query.value(0).toString(), query.value(1).toLongLong());
    }
    if (query.exec(QStringLiteral("SELECT received_ms,kind,http_status,code FROM events "
                                  "WHERE kind NOT IN ('accepted','data-recovered') ORDER BY id DESC LIMIT 1")) &&
        query.next()) {
        summary.insert(QStringLiteral("lastFailureUtcMs"), QString::number(query.value(0).toLongLong()));
        summary.insert(QStringLiteral("lastFailureKind"), query.value(1).toString());
        summary.insert(QStringLiteral("lastHttpStatus"), query.value(2).toInt());
        summary.insert(QStringLiteral("lastCode"), query.value(3).toInt());
    }
    if (query.exec(QStringLiteral("SELECT sent_ms,completed_ms,confirmed_ms FROM delivery "
                                  "WHERE completed_ms>0 ORDER BY completed_ms DESC LIMIT 1")) &&
        query.next()) {
        const qint64 sent = query.value(0).toLongLong();
        const qint64 completed = query.value(1).toLongLong();
        summary.insert(QStringLiteral("lastSentUtcMs"), QString::number(sent));
        summary.insert(QStringLiteral("lastCompletedUtcMs"), QString::number(completed));
        summary.insert(QStringLiteral("lastConfirmedUtcMs"), QString::number(query.value(2).toLongLong()));
        if (sent > 0 && completed >= sent && completed - sent <= 30000) {
            summary.insert(QStringLiteral("lastRoundTripMs"), completed - sent);
        }
    }
    return summary;
}
