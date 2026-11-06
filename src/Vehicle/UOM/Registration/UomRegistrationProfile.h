#pragma once

#include "UomRegistrationTypes.h"

/// Manufacturer connection settings and credential embedded in the B950 customer build.
class UomRegistrationProfile
{
public:
    [[nodiscard]] static bool customerBuild();
    [[nodiscard]] static UomRegistration::Config config();
    [[nodiscard]] static QString keyId();
};
