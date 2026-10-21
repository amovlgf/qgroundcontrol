#pragma once

#include <QtCore/QObject>

#include "UomTypes.h"

/// Provider callbacks are correlated with the immutable vehicle session generation and request sequence.
class UomProvider : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual Uom::Environment environment() const = 0;
    virtual void authenticate(quint64 generation, const Uom::Identity& identity) = 0;
    virtual void submit(quint64 generation, quint64 sequence, const Uom::Identity& identity,
                        const Uom::Telemetry& telemetry) = 0;
    virtual void cancel() = 0;

signals:
    void authenticated(quint64 generation, bool accepted, Uom::Fault fault, const QString& detail);
    void acknowledged(quint64 generation, quint64 sequence, bool accepted, Uom::Fault fault, const QString& detail);
};

/// Formal UOM credentials and protocol have not been supplied. This provider never claims readiness.
class UnconfiguredUomProvider final : public UomProvider
{
public:
    using UomProvider::UomProvider;

    Uom::Environment environment() const override { return Uom::Environment::Unconfigured; }

    void authenticate(quint64 generation, const Uom::Identity& identity) override;
    void submit(quint64 generation, quint64 sequence, const Uom::Identity& identity,
                const Uom::Telemetry& telemetry) override;

    void cancel() override {}
};
