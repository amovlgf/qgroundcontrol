#pragma once

#include "BaseClasses/VehicleTest.h"

class VehicleRidManagerTest : public VehicleTest
{
    Q_OBJECT
private slots:
    void _parameterRoundTripAndValidation();
    void _discoveryDiagnostics_data();
    void _discoveryDiagnostics();
    void _activationRoundTrip_data();
    void _activationRoundTrip();
    void _activationIdentityGuard_data();
    void _activationIdentityGuard();
    void _activationOutcomeFailure_data();
    void _activationOutcomeFailure();
    void _activationConcurrentRequestIsolation();
    void _identityAndSessionIsolation();
    void _maintenanceAndGroundRestriction();
    void _parameterValueBoundaries();
    void _credentialRequestsAreNotLogged();
    void _statusDigestBinding();
    void _factoryProvisionRoundTrip_data();
    void _factoryProvisionRoundTrip();
    void _factoryProvisionManagerRecreation();
    void _factoryProvisionRefusal_data();
    void _factoryProvisionRefusal();
    void _factoryProvisionVerification_data();
    void _factoryProvisionVerification();
};
