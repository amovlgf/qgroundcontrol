#pragma once

#include <QtCore/QObject>

#include "RidActivationProvider.h"

class RidActivationSyncController final : public QObject
{
    Q_OBJECT
public:
    explicit RidActivationSyncController(QObject* parent = nullptr);
    RidActivationSyncController(const QUrl& endpoint, QObject* parent);
    void begin(quint64 generation, const QString& serial);
    void cancel();

signals:
    void targetReady(quint64 generation, const QString& serial, bool active);
    void failed(const QString& detail);

private:
    RidActivationProvider _provider;
    quint64 _generation = 0;
    QString _serial;
    bool _querying = false;
};
