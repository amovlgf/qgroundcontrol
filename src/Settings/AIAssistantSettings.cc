/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "AIAssistantSettings.h"

#include <QtQml/QQmlEngine>

// Keep the legacy QSettings group so existing experimental-build credentials and
// model selections survive the UI/controller rename.
DECLARE_SETTINGGROUP(AIAssistant, "AIConsole")
{
    qmlRegisterUncreatableType<AIAssistantSettings>("QGroundControl.SettingsManager", 1, 0, "AIAssistantSettings", "Reference only");
}

DECLARE_SETTINGSFACT(AIAssistantSettings, authMethod)
DECLARE_SETTINGSFACT(AIAssistantSettings, endpointUrl)
DECLARE_SETTINGSFACT(AIAssistantSettings, modelName)
DECLARE_SETTINGSFACT(AIAssistantSettings, chatGptModelName)
DECLARE_SETTINGSFACT(AIAssistantSettings, apiKey)
DECLARE_SETTINGSFACT(AIAssistantSettings, oauthDeviceAuthorizationUrl)
DECLARE_SETTINGSFACT(AIAssistantSettings, oauthTokenUrl)
DECLARE_SETTINGSFACT(AIAssistantSettings, oauthClientId)
DECLARE_SETTINGSFACT(AIAssistantSettings, oauthScope)
