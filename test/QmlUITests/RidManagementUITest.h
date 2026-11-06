#pragma once

#include "VehicleConfigUITestBase.h"

class RidManagementUITest : public VehicleConfigUITestBase
{
    Q_OBJECT
private slots:
    void _setupAndToolbarRoutes();
    void _uomReportingStateSeparation();
    void _uomConfigurationAndCancellation();
    void _uomCustomerProfile_data();
    void _uomCustomerProfile();
    void _ridDialogIdentityInvalidation_data();
    void _ridDialogIdentityInvalidation();
};
