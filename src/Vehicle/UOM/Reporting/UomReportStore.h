#pragma once

#include <optional>

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtSql/QSqlDatabase>

#include "UomReportProtocol.h"

namespace UomReporting {

struct Capture
{
    Profile profile;
    Sample sample;
    QJsonObject identity;
    QJsonObject provenance;
    quint64 bootId = 0;
    quint32 flightSequence = 0;
    quint64 takeoffUtcMs = 0;
    quint64 connection = 0;
    bool airborne = false;
    bool valid = false;
    QString issue;
};

struct Delivery
{
    qint64 id = 0;
    Profile profile;
    QString aircraftId;
    QString orderId;
    QByteArray body;
    qint64 sampledMs = 0;
    int attempts = 0;
    quint64 bootId = 0;
    quint32 flightSequence = 0;
    QJsonObject identity;
    quint64 connection = 0;
};

struct QueueStats
{
    qint64 pending = 0;
    qint64 rejected = 0;
    qint64 held = 0;
    qint64 accepted = 0;
    qint64 total = 0;
};

}  // namespace UomReporting

/// Used exclusively on the reporting worker; snapshots are immutable and never contain credentials.
class UomReportStore final
{
public:
    explicit UomReportStore(const QString& path);
    ~UomReportStore();
    UomReportStore(const UomReportStore&) = delete;
    UomReportStore& operator=(const UomReportStore&) = delete;

    [[nodiscard]] bool ready() const { return _ready; }

    [[nodiscard]] QString error() const { return _error; }

    [[nodiscard]] std::optional<qint64> append(const UomReporting::Capture& capture, qint64 nowMs);
    [[nodiscard]] QList<UomReporting::Delivery> pending(qint64 nowMs, int limit = 128,
                                                        const QHash<QString, UomReporting::Profile>& profiles = {});
    [[nodiscard]] bool reserve(qint64 id, qint64 nowMs);
    [[nodiscard]] bool finish(qint64 id, const UomReporting::Result& result, qint64 nextAttemptMs);
    [[nodiscard]] QStringList aircraftIds();
    [[nodiscard]] UomReporting::QueueStats stats(const QString& aircraftId = {});
    [[nodiscard]] bool checkpoint();
    [[nodiscard]] bool recordEvent(const QString& kind, const QString& aircraftId, qint64 receivedMs,
                                   int httpStatus = 0, int code = 0, qint64 count = 1);
    [[nodiscard]] QJsonObject eventSummary();

private:
    bool _initialize();
    bool _fail();
    std::optional<QString> _orderId(const UomReporting::Capture& capture);
    QSqlDatabase _database;
    QString _connection;
    QString _error;
    QString _path;
    bool _ready = false;
};
