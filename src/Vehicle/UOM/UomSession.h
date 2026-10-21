#pragma once

#include "RuntimeScheduler.h"
#include "UomProvider.h"

/// One immutable aircraft identity per session. Reset invalidates all late callbacks.
class UomSession final : public QObject
{
    Q_OBJECT
public:
    explicit UomSession(UomProvider* provider, QObject* parent = nullptr, RuntimeScheduler* scheduler = nullptr);
    ~UomSession() override;
    void setIdentity(const Uom::Identity& identity);
    void setTelemetry(const Uom::Telemetry& telemetry);
    void reset();

    Uom::Environment environment() const { return _provider->environment(); }

    Uom::State state() const { return _state; }

    Uom::Fault fault() const { return _fault; }

    QString detail() const { return _detail; }

    quint64 generation() const { return _generation; }

    const Uom::Identity& identity() const { return _identity; }

    bool telemetryValid() const { return _telemetry.valid(_scheduler->nowUs()); }

    bool authenticated() const { return _authenticated; }

    bool acknowledged() const;

    bool officialReady() const { return environment() == Uom::Environment::Official && acknowledged(); }

    bool simulationReady() const { return environment() == Uom::Environment::Mock && acknowledged(); }

    quint32 lastAckAgeMs() const;

    quint64 nowUs() const { return _scheduler->nowUs(); }

signals:
    void changed();

private:
    void _tick();
    void _update(Uom::State state, Uom::Fault fault, const QString& detail);
    void _recover(Uom::Fault fault, const QString& detail);
    void _cancel();
    UomProvider* _provider;
    RuntimeScheduler* _scheduler;
    RuntimeScheduler::TaskId _task = 0;
    Uom::Identity _identity;
    Uom::Telemetry _telemetry;
    Uom::State _state = Uom::State::WaitingIdentity;
    Uom::Fault _fault = Uom::Fault::None;
    QString _detail;
    quint64 _generation = 1;
    quint64 _sequence = 0;
    quint64 _pendingSequence = 0;
    quint64 _requestTime = 0;
    quint64 _nextRequest = 0;
    quint64 _lastAck = 0;
    unsigned _failures = 0;
    bool _authenticated = false;
    bool _pending = false;
};
