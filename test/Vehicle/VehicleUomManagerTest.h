#pragma once

#include "BaseClasses/VehicleTest.h"

class VehicleUomManagerTest : public VehicleTest
{
    Q_OBJECT
private slots:
    void _identityBindingAndMavlink2();
    void _identityTimeout();
    void _oldTelemetryDoesNotRenewFreshness();
    void _statusPanelLoads();
};
