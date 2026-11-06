#include "UomReportingService.h"

#include <algorithm>
#include <memory>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>

#include "UomRegistrationCredentials.h"
#include "UomReportClient.h"

namespace {
QString scope(const UomReporting::Profile& profile)
{
    return QString::number(static_cast<int>(profile.mode)) + QLatin1Char('/') + profile.appId;
}

bool sameManufacturer(const UomReporting::Profile& first, const UomReporting::Profile& second)
{
    return first.mode == second.mode && first.appId == second.appId && first.endpoint == second.endpoint &&
           first.uasModel == second.uasModel && first.uscCode == second.uscCode && first.platform == second.platform &&
           first.source == second.source;
}
}  // namespace

class UomReportingWorker final : public QObject
{
public:
    explicit UomReportingWorker(UomReportingService* owner)
        : _owner(owner)
    {
        _clock.start();
    }

    void initialize(const QString& path)
    {
        _store = std::make_unique<UomReportStore>(path);
        _client = new UomReportClient(this);
        connect(_client, &UomReportClient::completed, this,
                [this](quint64 requestId, const UomReporting::Result& result) { complete(requestId, result); });
        _timer = new QTimer(this);
        _timer->setInterval(100);
        connect(_timer, &QTimer::timeout, this, [this]() { tick(); });
        _timer->start();
        publish();
    }

    void configure(const QList<UomReportingService::Binding>& bindings, const QHash<QString, QString>& keys)
    {
        for (const auto& binding : bindings) {
            QString previousKey;
            for (const auto& previous : _bindings) {
                if (sameManufacturer(previous.profile, binding.profile)) {
                    previousKey = _keys.value(previous.profile.credentialReference);
                    break;
                }
            }
            if (previousKey != keys.value(binding.profile.credentialReference)) {
                _paused.remove(scope(binding.profile));
                _backoff.remove(scope(binding.profile));
            }
        }
        _bindings = bindings;
        _keys = keys;
    }

    bool capture(const UomReporting::Capture& capture, qint64 queueAgeMs)
    {
        if (!_store) {
            return false;
        }
        auto& channel = _channels[capture.sample.aircraftId];
        const bool previouslyConnected = channel.connected;
        const bool previouslyValid = channel.capture.valid;
        const QString previousIssue = channel.capture.issue;
        if (channel.capture.connection != capture.connection || channel.capture.bootId != capture.bootId ||
            channel.capture.flightSequence != capture.flightSequence || channel.capture.identity != capture.identity ||
            !sameManufacturer(channel.capture.profile, capture.profile)) {
            channel = {};
        }
        channel.capture = capture;
        const qint64 now = _clock.elapsed();
        // Queueing and database startup consume freshness; processing never renews a sample.
        channel.arrivalMs = now - qBound<qint64>(0, queueAgeMs, now);
        channel.connected = true;
        const auto stored = _store->append(capture, QDateTime::currentMSecsSinceEpoch());
        if (!stored) {
            channel.detail = _store->error();
        } else if (!capture.valid) {
            channel.detail = capture.issue;
        }
        const qint64 receivedMs = QDateTime::currentMSecsSinceEpoch();
        if (!capture.valid && (!previouslyConnected || previouslyValid || previousIssue != capture.issue)) {
            (void) _store->recordEvent(QStringLiteral("invalid-data"), capture.sample.aircraftId, receivedMs);
        } else if (capture.valid && previouslyConnected && !previouslyValid) {
            (void) _store->recordEvent(QStringLiteral("data-recovered"), capture.sample.aircraftId, receivedMs);
        }
        if (stored && *stored > 0) {
            const qint64 gap =
                capture.provenance.value(QStringLiteral("droppedCapturesBefore")).toString().toLongLong();
            if (gap > 0) {
                (void) _store->recordEvent(QStringLiteral("capture-gap"), capture.sample.aircraftId, receivedMs, 0, 0,
                                           gap);
            }
        }
        // Publish the committed queue before the facade releases its outstanding capture count.
        publish();
        return stored && (*stored > 0 || !capture.provenance.contains(QStringLiteral("droppedCapturesBefore")));
    }

    void disconnectAircraft(const QString& aircraftId, quint64 connection)
    {
        auto it = _channels.find(aircraftId);
        if (it != _channels.end() && it->capture.connection == connection && it->connected) {
            it->connected = false;
            if (_store) {
                (void) _store->recordEvent(QStringLiteral("disconnected"), aircraftId,
                                           QDateTime::currentMSecsSinceEpoch());
            }
        }
    }

    void stop()
    {
        if (_timer) {
            _timer->stop();
        }
        if (_client) {
            _client->cancel();
        }
        if (_store) {
            (void) _store->checkpoint();
            _store.reset();
        }
    }

private:
    struct Channel
    {
        UomReporting::Capture capture;
        qint64 arrivalMs = 0;
        qint64 lastAckMs = -1;
        qint64 firstSendMs = -1;
        qint64 lastAckSampleMs = 0;
        bool connected = false;
        bool rejected = false;
        QString detail;
    };

    struct Pending
    {
        UomReporting::Delivery delivery;
        quint64 connection = 0;
        quint32 flightSequence = 0;
        bool live = false;
    };

    std::optional<UomReportingService::Binding> binding(const UomReporting::Delivery& item) const
    {
        for (const auto& configured : _bindings) {
            if (sameManufacturer(configured.profile, item.profile) &&
                configured.aircraftIds.contains(item.aircraftId)) {
                return configured;
            }
        }
        return std::nullopt;
    }

    void tick()
    {
        if (!_store || !_store->ready()) {
            publish();
            return;
        }
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 now = _clock.elapsed();
        QHash<QString, int> perAircraft;
        QSet<QString> accountBackfill;
        for (const Pending& pending : _pending) {
            ++perAircraft[pending.delivery.aircraftId];
            if (!pending.live) {
                accountBackfill.insert(scope(pending.delivery.profile));
            }
        }
        QHash<QString, UomReporting::Profile> eligibleProfiles;
        for (const auto& configured : _bindings) {
            const QString account = scope(configured.profile);
            if (!_keys.value(configured.profile.credentialReference).isEmpty() && !_paused.contains(account) &&
                _backoff.value(account) <= now) {
                for (const QString& aircraftId : configured.aircraftIds) {
                    eligibleProfiles.insert(aircraftId, configured.profile);
                }
            }
        }
        const auto due = eligibleProfiles.isEmpty() ? QList<UomReporting::Delivery>()
                                                    : _store->pending(nowMs, 128, eligibleProfiles);
        // One slot per aircraft per pass keeps a busy aircraft from monopolizing the application limit.
        QSet<QString> scheduled;
        for (auto item : due) {
            if (_pending.size() >= 32) {
                break;
            }
            const auto configured = binding(item);
            const QString account = scope(item.profile);
            if (!configured || _paused.contains(account) || _backoff.value(account) > now ||
                perAircraft.value(item.aircraftId) >= 8 || scheduled.contains(item.aircraftId)) {
                continue;
            }
            const QString key = _keys.value(configured->profile.credentialReference);
            if (key.isEmpty()) {
                continue;
            }
            auto channel = _channels.find(item.aircraftId);
            const bool live = channel != _channels.end() && channel->connected && channel->capture.airborne &&
                              channel->capture.valid && now - channel->arrivalMs <= 1000 && item.attempts == 0 &&
                              item.bootId == channel->capture.bootId && item.connection != 0 &&
                              item.connection == channel->capture.connection &&
                              item.flightSequence == channel->capture.flightSequence &&
                              item.identity == channel->capture.identity &&
                              sameManufacturer(item.profile, channel->capture.profile) &&
                              item.sampledMs <= qint64(channel->capture.sample.timestampMs) &&
                              qint64(channel->capture.sample.timestampMs) - item.sampledMs <= 1000;
            if (!live && (!configured->profile.backfillValidated || accountBackfill.contains(account) ||
                          _nextBackfill.value(account) > now)) {
                continue;
            }
            item.profile.credentialReference = configured->profile.credentialReference;
            if (!_store->reserve(item.id, nowMs)) {
                break;
            }
            Pending pending{item, 0, 0, live};
            if (live) {
                pending.connection = channel->capture.connection;
                pending.flightSequence = channel->capture.flightSequence;
                if (channel->firstSendMs < 0) {
                    channel->firstSendMs = now;
                }
            } else {
                accountBackfill.insert(account);
                _nextBackfill[account] = now + 1000;
            }
            _pending.insert(item.id, pending);
            ++perAircraft[item.aircraftId];
            scheduled.insert(item.aircraftId);
            _client->send(item.id, item.profile, key, item.orderId, item.body);
        }
        if (now - _lastPublishMs >= 500) {
            publish();
        }
    }

    void complete(quint64 id, const UomReporting::Result& result)
    {
        auto it = _pending.find(id);
        if (it == _pending.end() || !_store) {
            return;
        }
        const Pending pending = it.value();
        _pending.erase(it);
        const QString account = scope(pending.delivery.profile);
        const qint64 now = _clock.elapsed();
        const qint64 delay = std::min(30000, 1000 << std::min(pending.delivery.attempts, 5));
        if (!_store->finish(id, result, QDateTime::currentMSecsSinceEpoch() + delay)) {
            publish();
            return;
        }
        const QString event = result.success                 ? QStringLiteral("accepted")
                              : result.authenticationFailure ? QStringLiteral("authentication")
                              : result.retryable             ? QStringLiteral("retryable")
                                                             : QStringLiteral("rejected");
        (void) _store->recordEvent(event, pending.delivery.aircraftId, QDateTime::currentMSecsSinceEpoch(),
                                   result.httpStatus, result.code);
        if (result.authenticationFailure) {
            _paused.insert(account);
        } else if (result.retryable) {
            _backoff[account] = now + delay;
        }
        auto channel = _channels.find(pending.delivery.aircraftId);
        if (pending.live && channel != _channels.end() && channel->connected &&
            channel->capture.connection == pending.connection &&
            channel->capture.flightSequence == pending.flightSequence &&
            channel->capture.bootId == pending.delivery.bootId &&
            channel->capture.identity == pending.delivery.identity &&
            sameManufacturer(channel->capture.profile, pending.delivery.profile) && now - channel->arrivalMs <= 1000 &&
            channel->capture.valid && channel->capture.airborne &&
            qint64(channel->capture.sample.timestampMs) >= pending.delivery.sampledMs &&
            qint64(channel->capture.sample.timestampMs) - pending.delivery.sampledMs <= 1000) {
            if (result.success && pending.delivery.sampledMs >= channel->lastAckSampleMs) {
                channel->lastAckMs = now;
                channel->lastAckSampleMs = pending.delivery.sampledMs;
                channel->rejected = false;
                channel->detail.clear();
            } else if (!result.success) {
                channel->rejected = !result.retryable;
                channel->detail = result.detail;
            }
        }
        publish();
    }

    void publish()
    {
        if (!_store) {
            return;
        }
        _lastPublishMs = _clock.elapsed();
        QHash<QString, QVariantMap> states;
        const auto storedAircraft = _store->aircraftIds();
        QSet<QString> aircraft(storedAircraft.cbegin(), storedAircraft.cend());
        for (const auto& configured : _bindings) {
            aircraft.unite(QSet<QString>(configured.aircraftIds.cbegin(), configured.aircraftIds.cend()));
        }
        for (auto it = _channels.cbegin(); it != _channels.cend(); ++it) {
            aircraft.insert(it.key());
        }
        for (const QString& aircraftId : aircraft) {
            const auto channel = _channels.value(aircraftId);
            const auto stats = _store->stats(aircraftId);
            const qint64 age = channel.lastAckMs < 0 ? -1 : _lastPublishMs - channel.lastAckMs;
            const bool healthy = _store->ready() && !_paused.contains(scope(channel.capture.profile)) &&
                                 !channel.rejected && channel.detail.isEmpty() && channel.connected &&
                                 channel.capture.valid && channel.capture.airborne &&
                                 _lastPublishMs - channel.arrivalMs <= 1000 && age >= 0 && age <= 5000;
            const bool timeout = channel.connected && channel.capture.airborne && channel.firstSendMs >= 0 &&
                                 _lastPublishMs - std::max(channel.firstSendMs, channel.lastAckMs) > 5000;
            states.insert(aircraftId, {{"healthy", healthy},
                                       {"ackAgeMs", age},
                                       {"started", channel.firstSendMs >= 0},
                                       {"networkFault", timeout || !channel.detail.isEmpty()},
                                       {"rejected", channel.rejected},
                                       {"detail", channel.detail},
                                       {"pending", stats.pending},
                                       {"held", stats.held},
                                       {"accepted", stats.accepted},
                                       {"rejectedCount", stats.rejected},
                                       {"total", stats.total}});
        }
        const auto totals = _store->stats();
        const bool ready = _store->ready();
        const auto events = _store->eventSummary();
        QPointer<UomReportingService> owner = _owner;
        QMetaObject::invokeMethod(_owner, [owner, states, ready, totals, events]() {
            if (owner) {
                owner->_receive(states, ready, totals.pending, totals.held, events);
            }
        });
    }

    UomReportingService* _owner;
    std::unique_ptr<UomReportStore> _store;
    UomReportClient* _client = nullptr;
    QTimer* _timer = nullptr;
    QElapsedTimer _clock;
    QList<UomReportingService::Binding> _bindings;
    QHash<QString, QString> _keys;
    QHash<QString, Channel> _channels;
    QHash<quint64, Pending> _pending;
    QSet<QString> _paused;
    QHash<QString, qint64> _backoff;
    QHash<QString, qint64> _nextBackfill;
    qint64 _lastPublishMs = 0;
};

UomReportingService* UomReportingService::instance()
{
    static QPointer<UomReportingService> service;
    if (!service) {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                             QStringLiteral("/uom-reporting/reports.sqlite");
        service = new UomReportingService(path, QCoreApplication::instance());
        service->reloadConfiguration();
    }
    return service;
}

UomReportingService::UomReportingService(const QString& databasePath, QObject* parent)
    : QObject(parent)
    , _worker(new UomReportingWorker(this))
    , _credentials(new UomRegistrationCredentials(this))
{
    _worker->moveToThread(&_thread);
    connect(&_thread, &QThread::finished, _worker, &QObject::deleteLater);
    connect(&_thread, &QThread::started, _worker,
            [worker = _worker, databasePath]() { worker->initialize(databasePath); });
    connect(_credentials, &UomRegistrationCredentials::completed, this,
            [this](const QString&, bool, bool, const QString&) {
                if (_stopped || !_worker) {
                    return;
                }
                QHash<QString, QString> keys;
                for (const auto& binding : _bindings) {
                    const QString reference = binding.profile.credentialReference;
                    if (credentialAvailable(reference)) {
                        keys.insert(reference, _credentials->value(reference));
                    }
                }
                QMetaObject::invokeMethod(
                    _worker, [worker = _worker, bindings = _bindings, keys]() { worker->configure(bindings, keys); });
                emit changed();
            });
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &UomReportingService::shutdown);
    }
    _thread.start();
}

UomReportingService::~UomReportingService()
{
    shutdown();
}

std::optional<UomReportingService::Binding> UomReportingService::binding(const QString& aircraftId) const
{
    for (const auto& binding : _bindings) {
        if (binding.aircraftIds.contains(aircraftId)) {
            return binding;
        }
    }
    return std::nullopt;
}

bool UomReportingService::credentialAvailable(const QString& reference) const
{
    for (const auto& binding : _bindings) {
        if (binding.profile.mode == UomReporting::Mode::Isolated && binding.profile.credentialReference == reference &&
            !_isolatedKeys.value(reference).isEmpty()) {
            return true;
        }
    }
    return _credentials->persistent(reference) && !_credentials->value(reference).isEmpty();
}

bool UomReportingService::ready() const
{
    return !_stopped && !_captureFault && _queuedCaptures < 64 && _storageReady && _heartbeat.isValid() &&
           _heartbeat.elapsed() <= 3000;
}

QVariantMap UomReportingService::status(const QString& aircraftId) const
{
    QVariantMap state = _status.value(aircraftId);
    state.insert(QStringLiteral("captureFault"), _captureFault);
    state.insert(QStringLiteral("droppedCaptures"), _droppedCaptures);
    if (_captureFault) {
        state.insert(QStringLiteral("healthy"), false);
        state.insert(QStringLiteral("networkFault"), true);
        state.insert(QStringLiteral("detail"), tr("Reporting storage queue overflowed; a data gap is recorded"));
    }
    return state;
}

bool UomReportingService::hasOutstandingWork() const
{
    return _pending > 0 || _held > 0 || _queuedCaptures > 0 || _unrecordedDropped > 0;
}

void UomReportingService::_receive(const QHash<QString, QVariantMap>& status, bool storageReady, qint64 pending,
                                   qint64 held, const QJsonObject& events)
{
    _status = status;
    _events = events;
    _storageReady = storageReady;
    _pending = pending;
    _held = held;
    _heartbeat.start();
    emit changed();
}

void UomReportingService::capture(const UomReporting::Capture& capture)
{
    if (_stopped || !_worker) {
        return;
    }
    if (_queuedCaptures >= 64) {
        ++_droppedCaptures;
        ++_unrecordedDropped;
        _captureFault = true;
        emit changed();
        return;
    }
    QElapsedTimer queued;
    queued.start();
    auto snapshot = capture;
    const qint64 gap = _unrecordedDropped;
    if (gap > 0) {
        snapshot.provenance.insert(QStringLiteral("droppedCapturesBefore"), QString::number(gap));
        _unrecordedDropped = 0;
    }
    ++_queuedCaptures;
    QPointer<UomReportingService> owner(this);
    QMetaObject::invokeMethod(_worker, [worker = _worker, owner, snapshot, gap, queued]() {
        const bool stored = worker->capture(snapshot, queued.elapsed());
        if (owner) {
            QMetaObject::invokeMethod(owner, [owner, stored, gap]() {
                if (!owner) {
                    return;
                }
                --owner->_queuedCaptures;
                if (gap > 0 && stored && owner->_unrecordedDropped == 0) {
                    owner->_captureFault = false;
                } else if (gap > 0 && !stored) {
                    owner->_unrecordedDropped += gap;
                }
                emit owner->changed();
            });
        }
    });
}

void UomReportingService::disconnectAircraft(const QString& aircraftId, quint64 connection)
{
    if (!_stopped) {
        QMetaObject::invokeMethod(_worker, [worker = _worker, aircraftId, connection]() {
            worker->disconnectAircraft(aircraftId, connection);
        });
    }
}

void UomReportingService::reloadConfiguration()
{
    if (_stopped) {
        return;
    }
    _bindings.clear();
    _isolatedKeys.clear();
    _configurationError.clear();
    const QString path = QSettings().value(QStringLiteral("UomReporting/profileFile")).toString();
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 65536) {
        _configurationError = tr("Manufacturer reporting profile is not configured");
    } else {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        const QJsonArray profiles = document.object().value(QStringLiteral("profiles")).toArray();
        QSet<QString> aircraft;
        for (const QJsonValue& value : profiles) {
            const QJsonObject object = value.toObject();
            const auto profile = UomReporting::Profile::fromJson(object.value(QStringLiteral("profile")).toObject());
            Binding binding;
            const int classification = object.value(QStringLiteral("aircraftClass")).toInt(-1);
            const int category = object.value(QStringLiteral("operationCategory")).toInt(-1);
            if (!profile || classification < 0 || classification > 4 || category < 0 || category > 3 ||
                !profile->credentialReference.startsWith(QStringLiteral("uom-reporting/"))) {
                _configurationError = tr("Manufacturer reporting profile is invalid");
                break;
            }
            binding.profile = *profile;
            binding.aircraftClass = classification;
            binding.operationCategory = category;
            for (const QJsonValue& id : object.value(QStringLiteral("aircraftIds")).toArray()) {
                const QString serial = id.toString();
                if (serial.size() != 20 || aircraft.contains(serial)) {
                    _configurationError = tr("Reporting aircraft bindings are invalid or duplicated");
                    break;
                }
                aircraft.insert(serial);
                binding.aircraftIds.append(serial);
            }
            if (binding.aircraftIds.isEmpty() || !_configurationError.isEmpty()) {
                _configurationError = tr("An explicit aircraft binding is required for reporting");
                break;
            }
            _bindings.append(binding);
        }
        if (_bindings.isEmpty() && _configurationError.isEmpty()) {
            _configurationError = tr("Manufacturer reporting profile is empty");
        }
    }
    if (!_configurationError.isEmpty()) {
        _bindings.clear();
    }
    QMetaObject::invokeMethod(_worker, [worker = _worker, bindings = _bindings]() { worker->configure(bindings, {}); });
    for (const auto& binding : _bindings) {
        _credentials->getPersistent(binding.profile.credentialReference, true);
    }
    emit changed();
}

void UomReportingService::shutdown()
{
    if (_stopped) {
        return;
    }
    _stopped = true;
    QMetaObject::invokeMethod(_worker, [worker = _worker]() { worker->stop(); }, Qt::BlockingQueuedConnection);
    _thread.quit();
    _thread.wait();
    _worker = nullptr;
}

bool UomReportingService::exportDiagnostics(const QString& path) const
{
    QJsonArray aircraft;
    for (auto it = _status.cbegin(); it != _status.cend(); ++it) {
        QVariantMap status = it.value();
        status.remove(QStringLiteral("detail"));
        aircraft.append(QJsonObject::fromVariantMap(status));
    }
    const QJsonObject report{{"schemaVersion", 1},
                             {"protocol", "UOM-V2.3/0x20"},
                             {"applicationVersion", QCoreApplication::applicationVersion()},
                             {"storageReady", ready()},
                             {"pending", _pending},
                             {"held", _held},
                             {"captureFault", _captureFault},
                             {"queuedCaptures", _queuedCaptures},
                             {"droppedCaptures", _droppedCaptures},
                             {"unrecordedDropped", _unrecordedDropped},
                             {"events", _events},
                             {"aircraft", aircraft}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(report).toJson();
    return file.write(bytes) == bytes.size() && file.commit();
}

void UomReportingService::_configureIsolated(const QList<Binding>& bindings, const QHash<QString, QString>& keys)
{
    if (_stopped || bindings.isEmpty()) {
        return;
    }
    for (const auto& binding : bindings) {
        if (binding.profile.mode != UomReporting::Mode::Isolated || !binding.profile.valid()) {
            return;
        }
    }
    _bindings = bindings;
    _isolatedKeys = keys;
    QMetaObject::invokeMethod(_worker, [worker = _worker, bindings, keys]() { worker->configure(bindings, keys); });
}
