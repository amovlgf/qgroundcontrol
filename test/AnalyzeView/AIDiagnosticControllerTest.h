/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "UnitTest.h"

class AIDiagnosticControllerTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _assistantPagesLoad_test();
    void _settingsCompatibilityAlias_test();
    void _contextSchemaWithoutVehicle_test();
    void _responseLanguagePolicy_test();
    void _px4ProviderBoundaryAndPolicy_test();
    void _vehicleSwitchAndStaleEventIsolation_test();
    void _externalVisionInputPresentButNotConfigured_test();
    void _externalVisionFusionControl_test();
    void _externalVisionInvalidPayload_test();
    void _externalVisionUnavailableAndUnknown_test();
    void _externalVisionRoutingAndSafety_test();
};
