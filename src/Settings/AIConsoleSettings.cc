/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "AIConsoleSettings.h"

#include <QtQml/QQmlEngine>

DECLARE_SETTINGGROUP(AIConsole, "AIConsole")
{
    qmlRegisterUncreatableType<AIConsoleSettings>("QGroundControl.SettingsManager", 1, 0, "AIConsoleSettings", "Reference only");
}

DECLARE_SETTINGSFACT(AIConsoleSettings, authMethod)
DECLARE_SETTINGSFACT(AIConsoleSettings, endpointUrl)
DECLARE_SETTINGSFACT(AIConsoleSettings, modelName)
DECLARE_SETTINGSFACT(AIConsoleSettings, chatGptModelName)
DECLARE_SETTINGSFACT(AIConsoleSettings, apiKey)
DECLARE_SETTINGSFACT(AIConsoleSettings, oauthDeviceAuthorizationUrl)
DECLARE_SETTINGSFACT(AIConsoleSettings, oauthTokenUrl)
DECLARE_SETTINGSFACT(AIConsoleSettings, oauthClientId)
DECLARE_SETTINGSFACT(AIConsoleSettings, oauthScope)
