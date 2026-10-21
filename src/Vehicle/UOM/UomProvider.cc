#include "UomProvider.h"

void UnconfiguredUomProvider::authenticate(quint64 generation, const Uom::Identity& /*identity*/)
{
    emit authenticated(generation, false, Uom::Fault::Authentication, tr("Official UOM is not configured"));
}

void UnconfiguredUomProvider::submit(quint64 generation, quint64 sequence, const Uom::Identity& /*identity*/,
                                     const Uom::Telemetry& /*telemetry*/)
{
    emit acknowledged(generation, sequence, false, Uom::Fault::Authentication, tr("Official UOM is not configured"));
}
