#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QThread>
#include <QtCore/QVariantMap>

#include "UomReportStore.h"

class UomReportingWorker;
class UomRegistrationCredentials;

/// Application-owned reporting survives the lifetime of any Vehicle or view.
class UomReportingService final : public QObject
{
    Q_OBJECT
public:
    struct Binding
    {
        UomReporting::Profile profile;
        QStringList aircraftIds;
        quint8 aircraftClass = 0;
        quint8 operationCategory = 0;
    };

    static UomReportingService* instance();
    explicit UomReportingService(const QString& databasePath, QObject* parent = nullptr);
    ~UomReportingService() override;
    [[nodiscard]] std::optional<Binding> binding(const QString& aircraftId) const;
    [[nodiscard]] bool credentialAvailable(const QString& reference) const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] QVariantMap status(const QString& aircraftId) const;
    [[nodiscard]] bool hasOutstandingWork() const;

    [[nodiscard]] QString configurationError() const { return _configurationError; }

    void capture(const UomReporting::Capture& capture);
    void disconnectAircraft(const QString& aircraftId, quint64 connection);
    void reloadConfiguration();
    void shutdown();
    [[nodiscard]] bool exportDiagnostics(const QString& path) const;

signals:
    void changed();

private:
    friend class UomReportingWorker;
    friend class UomReportingServiceTest;
    friend class VehicleUomManagerTest;
    void _configureIsolated(const QList<Binding>& bindings, const QHash<QString, QString>& keys);
    void _receive(const QHash<QString, QVariantMap>& status, bool storageReady, qint64 pending, qint64 held,
                  const QJsonObject& events);
    QThread _thread;
    UomReportingWorker* _worker = nullptr;
    UomRegistrationCredentials* _credentials = nullptr;
    QList<Binding> _bindings;
    QHash<QString, QString> _isolatedKeys;
    QHash<QString, QVariantMap> _status;
    QJsonObject _events;
    QElapsedTimer _heartbeat;
    QString _configurationError;
    qint64 _pending = 0;
    qint64 _held = 0;
    int _queuedCaptures = 0;
    qint64 _droppedCaptures = 0;
    qint64 _unrecordedDropped = 0;
    bool _captureFault = false;
    bool _storageReady = false;
    bool _stopped = false;
};
