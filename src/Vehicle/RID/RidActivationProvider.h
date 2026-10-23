#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>

class QNetworkReply;

struct RidActivationResult
{
    enum class State
    {
        Unknown,
        Unbound,
        Active,
        Inactive
    };
    State state = State::Unknown;
    QString detail;

    bool writable() const { return state == State::Active || state == State::Inactive; }
};
Q_DECLARE_METATYPE(RidActivationResult)

/// Queries the platform's existing state; never registers or activates a device on the platform.
class RidActivationProvider final : public QObject
{
    Q_OBJECT
public:
    explicit RidActivationProvider(QObject* parent = nullptr);
    RidActivationProvider(const QUrl& endpoint, QObject* parent);
    void query(quint64 generation, const QString& serial);
    void cancel();
    static bool validSerial(const QString& serial);
    static RidActivationResult parseResponse(const QString& serial, int httpStatus, const QByteArray& body);

signals:
    void resultReady(quint64 generation, const RidActivationResult& result);

private:
    QNetworkAccessManager _network;
    QUrl _endpoint;
    QPointer<QNetworkReply> _reply;
    quint64 _generation = 0;
};
