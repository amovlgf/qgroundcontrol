#pragma once

#include "VehicleConfigUITestBase.h"

class RidManagementUITest : public VehicleConfigUITestBase
{
    Q_OBJECT
private slots:
    void _setupAndToolbarRoutes();
    void _uomConfigurationAndCancellation();
};
