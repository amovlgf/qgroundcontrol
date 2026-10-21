#include "UomSession.h"

#include <algorithm>
#include <limits>

UomSession::UomSession(UomProvider* provider, QObject* parent, RuntimeScheduler* scheduler)
    : QObject(parent)
    , _provider(provider)
    , _scheduler(RuntimeScheduler::orDefault(scheduler, this))
{
    connect(_provider, &UomProvider::authenticated, this,
            [this](quint64 generation, bool accepted, Uom::Fault fault, const QString& detail) {
                if (generation != _generation || !_pending || _pendingSequence != 0) {
                    return;
                }
                _pending = false;
                if (!accepted) {
                    _recover(fault, detail);
                    return;
                }
                _authenticated = true;
                _nextRequest = 0;
                _update(Uom::State::Reporting, Uom::Fault::None, detail);
            });
    connect(_provider, &UomProvider::acknowledged, this,
            [this](quint64 generation, quint64 sequence, bool accepted, Uom::Fault fault, const QString& detail) {
                if (generation != _generation || !_pending || sequence != _pendingSequence || sequence == 0) {
                    return;
                }
                _pending = false;
                if (!accepted) {
                    _recover(fault, detail);
                    return;
                }
                _lastAck = _scheduler->nowUs();
                _failures = 0;
                _update(Uom::State::Reporting, Uom::Fault::None, detail);
                emit changed();
            });
    _tick();
}

UomSession::~UomSession()
{
    _scheduler->cancel(_task);
    _provider->cancel();
}

void UomSession::_cancel()
{
    ++_generation;
    _provider->cancel();
    _pending = false;
    _authenticated = false;
    _lastAck = 0;
    _nextRequest = 0;
}

void UomSession::reset()
{
    _cancel();
    _identity = {};
    _telemetry = {};
    _failures = 0;
    _update(Uom::State::WaitingIdentity, Uom::Fault::Identity, tr("Waiting for aircraft identity"));
}

void UomSession::setIdentity(const Uom::Identity& identity)
{
    if (_identity == identity) {
        return;
    }
    _cancel();
    _identity = identity;
    _failures = 0;
    emit changed();
}

void UomSession::setTelemetry(const Uom::Telemetry& telemetry)
{
    _telemetry = telemetry;
}

bool UomSession::acknowledged() const
{
    return _identity.valid() && _identity.activated && telemetryValid() && _authenticated &&
           _state == Uom::State::Reporting && _lastAck != 0 && _scheduler->nowUs() - _lastAck <= Uom::ACK_TIMEOUT_US;
}

quint32 UomSession::lastAckAgeMs() const
{
    return _lastAck ? static_cast<quint32>(std::min<quint64>((_scheduler->nowUs() - _lastAck) / 1000,
                                                             std::numeric_limits<quint32>::max()))
                    : std::numeric_limits<quint32>::max();
}

void UomSession::_update(Uom::State state, Uom::Fault fault, const QString& detail)
{
    if (_state == state && _fault == fault && _detail == detail) {
        return;
    }
    _state = state;
    _fault = fault;
    _detail = detail;
    emit changed();
}

void UomSession::_recover(Uom::Fault fault, const QString& detail)
{
    _cancel();
    _nextRequest = _scheduler->nowUs() + (quint64(1) << std::min(_failures++, 2U)) * 1000000;
    _update(Uom::State::Recovering, fault, detail);
}

void UomSession::_tick()
{
    _task = _scheduler->schedule(this, std::chrono::milliseconds(100), [this]() { _tick(); });
    if (environment() == Uom::Environment::Unconfigured) {
        _update(Uom::State::NotConfigured, Uom::Fault::None, tr("Official UOM is not configured"));
        return;
    }
    if (!_identity.valid() || !_identity.activated) {
        _update(Uom::State::WaitingIdentity, _identity.valid() ? Uom::Fault::Activation : Uom::Fault::Identity,
                _identity.valid() ? tr("RID activation required") : tr("Waiting for valid aircraft identity"));
        return;
    }
    if (!telemetryValid()) {
        if (_pending || _authenticated) {
            _cancel();
        }
        _update(Uom::State::WaitingData, Uom::Fault::Telemetry,
                tr("Waiting for fresh aircraft and control station data"));
        return;
    }
    const quint64 now = _scheduler->nowUs();
    if (_pending) {
        if (now - _requestTime >= 3000000) {
            _recover(Uom::Fault::Timeout, tr("UOM request timed out"));
        }
        return;
    }
    if (now < _nextRequest) {
        return;
    }
    _pending = true;
    _requestTime = now;
    _nextRequest = now + Uom::REPORT_INTERVAL_US;
    if (!_authenticated) {
        _pendingSequence = 0;
        _update(Uom::State::Authenticating, Uom::Fault::None, tr("Authenticating with the simulation service"));
        _provider->authenticate(_generation, _identity);
    } else {
        _pendingSequence = ++_sequence;
        _provider->submit(_generation, _pendingSequence, _identity, _telemetry);
    }
}
