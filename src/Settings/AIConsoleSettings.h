/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "SettingsGroup.h"

class AIConsoleSettings : public SettingsGroup
{
    Q_OBJECT

public:
    explicit AIConsoleSettings(QObject *parent = nullptr);

    DEFINE_SETTING_NAME_GROUP()

    DEFINE_SETTINGFACT(endpointUrl)
    DEFINE_SETTINGFACT(modelName)
    DEFINE_SETTINGFACT(apiKey)
};
