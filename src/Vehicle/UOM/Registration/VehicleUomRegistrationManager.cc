#include "VehicleUomRegistrationManager.h"

#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>

#include "Fact.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VehicleRidManager.h"

namespace {
UomRegistrationDevice deviceSnapshot(const RidActivationContext& context, int state)
{
    return {context.serial, context.uid, context.boot, context.revision, context.generation, state};
}

QString databasePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("uom-registration.sqlite"));
}
}  // namespace

VehicleUomRegistrationManager::VehicleUomRegistrationManager(Vehicle* vehicle)
    : QObject(vehicle)
    , _vehicle(vehicle)
    , _store(std::make_unique<UomRegistrationStore>(databasePath()))
    , _credentials(this)
    , _client(_store.get(), this)
    , _workflow(_store.get(), &_client, &_credentials, this)
{
    connect(&_workflow, &UomRegistrationWorkflow::changed, this, &VehicleUomRegistrationManager::changed);
    if (!_vehicle || !_vehicle->ridManager()) {
        return;
    }
    auto* rid = _vehicle->ridManager();
    connect(rid, &VehicleRidManager::changed, this, &VehicleUomRegistrationManager::_syncDevice);
    connect(rid, &VehicleRidManager::activationRead, this,
            [this](quint64 id, const RidActivationContext& context, int state) {
                _workflow.deviceRead(id, deviceSnapshot(context, state));
            });
    connect(rid, &VehicleRidManager::activationWritten, this,
            [this](quint64 id, const RidActivationContext& context, int state) {
                _workflow.deviceWritten(id, deviceSnapshot(context, state));
            });
    connect(rid, &VehicleRidManager::activationFailed, &_workflow, &UomRegistrationWorkflow::deviceFailed);
    connect(&_workflow, &UomRegistrationWorkflow::readDeviceRequested, rid,
            [rid](quint64 id) { rid->readActivation(id); });
    connect(&_workflow, &UomRegistrationWorkflow::writeDeviceRequested, rid,
            [rid](quint64 id, const UomRegistrationDevice& expected, bool active) {
                rid->setActivation(
                    id, {expected.serial, expected.uid, expected.boot, expected.revision, expected.generation}, active);
            });
    connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, [this](Vehicle* active) {
        if (active != _vehicle) {
            _workflow.interrupt();
        }
        _syncDevice();
    });
    connect(_vehicle, &Vehicle::armedChanged, this, [this](bool armed) {
        if (armed) {
            _workflow.interrupt();
        }
        _syncDevice();
    });
    connect(_vehicle, &Vehicle::underwayChanged, this, [this](bool underway) {
        if (underway) {
            _workflow.interrupt();
        }
        _syncDevice();
    });
    connect(_vehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, [this](bool lost) {
        if (lost) {
            _workflow.interrupt();
        }
        _syncDevice();
    });
    connect(_vehicle->parameterManager(), &ParameterManager::parametersReadyChanged, this,
            &VehicleUomRegistrationManager::_watchSafetyFacts);
    _watchSafetyFacts();
    _loadConfiguration();
    _syncDevice();
}

void VehicleUomRegistrationManager::_syncDevice()
{
    if (!_vehicle || !_vehicle->ridManager()) {
        return;
    }
    auto* rid = _vehicle->ridManager();
    _workflow.observeDevice(deviceSnapshot(rid->activationContext(), rid->activationState()), rid->canWrite());
}

QVariantMap VehicleUomRegistrationManager::configurationForEnvironment(const QString& environment) const
{
    if (environment != QStringLiteral("integration") && environment != QStringLiteral("official")) {
        return {};
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("UomRegistration/") + environment);
    UomRegistration::Config defaults;
    const bool integration = environment == QStringLiteral("integration");
    return {{QStringLiteral("environment"), environment},
            {QStringLiteral("producerId"), settings.value(QStringLiteral("producerId"), QStringLiteral(""))},
            {QStringLiteral("expectedSerial"),
             settings.value(QStringLiteral("expectedSerial"),
                            integration ? QStringLiteral("ALABB950B950U4400001") : QString())},
            {QStringLiteral("verifyUrl"),
             settings.value(QStringLiteral("verifyUrl"), integration ? defaults.verifyUrl.toString() : QString())},
            {QStringLiteral("reportUrl"),
             settings.value(QStringLiteral("reportUrl"), integration ? defaults.reportUrl.toString() : QString())},
            {QStringLiteral("cancelUrl"),
             settings.value(QStringLiteral("cancelUrl"), integration ? defaults.cancelUrl.toString() : QString())}};
}

void VehicleUomRegistrationManager::_loadConfiguration()
{
    QSettings settings;
    const QString environment =
        settings.value(QStringLiteral("UomRegistration/environment"), QStringLiteral("integration")).toString();
    const auto values = configurationForEnvironment(environment);
    UomRegistration::Config config;
    config.environment = environment;
    config.producerId = values.value(QStringLiteral("producerId")).toString();
    config.expectedSerial = values.value(QStringLiteral("expectedSerial")).toString();
    config.verifyUrl = QUrl(values.value(QStringLiteral("verifyUrl")).toString());
    config.reportUrl = QUrl(values.value(QStringLiteral("reportUrl")).toString());
    config.cancelUrl = QUrl(values.value(QStringLiteral("cancelUrl")).toString());
    if (config.valid()) {
        _workflow.configure(config);
    }
}

bool VehicleUomRegistrationManager::configure(const QString& environment, const QString& producerId,
                                              const QString& expectedSerial, const QString& verifyUrl,
                                              const QString& reportUrl, const QString& cancelUrl, const QString& keyId)
{
    UomRegistration::Config config;
    config.environment = environment;
    config.producerId = producerId.trimmed();
    config.expectedSerial = expectedSerial.trimmed();
    config.verifyUrl = QUrl(verifyUrl.trimmed());
    config.reportUrl = QUrl(reportUrl.trimmed());
    config.cancelUrl = QUrl(cancelUrl.trimmed());
    if (!_workflow.configure(config, keyId)) {
        return false;
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("UomRegistration"));
    settings.setValue(QStringLiteral("environment"), config.environment);
    settings.beginGroup(config.environment);
    settings.setValue(QStringLiteral("producerId"), config.producerId);
    settings.setValue(QStringLiteral("expectedSerial"), config.expectedSerial);
    settings.setValue(QStringLiteral("verifyUrl"), config.verifyUrl.toString());
    settings.setValue(QStringLiteral("reportUrl"), config.reportUrl.toString());
    settings.setValue(QStringLiteral("cancelUrl"), config.cancelUrl.toString());
    return true;
}

QString VehicleUomRegistrationManager::statusText() const
{
    if (!_store->ready()) {
        return tr("UOM transaction storage is unavailable; operations are disabled");
    }
    if (!busy() && _vehicle && _vehicle->ridManager() && !_vehicle->ridManager()->serial().isEmpty() &&
        _vehicle->ridManager()->serial() != expectedSerial()) {
        return tr("Connected product code does not match the configured aircraft; no operation is permitted");
    }
    return _workflow.statusText();
}

void VehicleUomRegistrationManager::_watchSafetyFacts()
{
    if (!_vehicle || !_vehicle->parameterManager()) {
        return;
    }
    auto* parameters = _vehicle->parameterManager();
    for (const QString& name : {QStringLiteral("COM_ARM_ODID"), QStringLiteral("COM_RID_UOM")}) {
        if (parameters->parameterExists(ParameterManager::defaultComponentId, name)) {
            if (auto* fact = parameters->getParameter(ParameterManager::defaultComponentId, name)) {
                connect(fact, &Fact::rawValueChanged, this, &VehicleUomRegistrationManager::changed,
                        Qt::UniqueConnection);
            }
        }
    }
    emit changed();
}

QString VehicleUomRegistrationManager::safetyStatus() const
{
    if (!_vehicle || !_vehicle->parameterManager()) {
        return tr("Connect an aircraft to check its existing flight restrictions");
    }
    auto* parameters = _vehicle->parameterManager();
    const auto read = [parameters](const QString& name) {
        if (!parameters->parameterExists(ParameterManager::defaultComponentId, name)) {
            return -1;
        }
        const auto* fact = parameters->getParameter(ParameterManager::defaultComponentId, name);
        return fact ? fact->rawValue().toInt() : -1;
    };
    const int odid = read(QStringLiteral("COM_ARM_ODID"));
    const int uom = read(QStringLiteral("COM_RID_UOM"));
    QString status = odid >= 2
                         ? tr("The current flight-controller setting requires a healthy RID; verify matching RID "
                              "firmware on hardware")
                         : tr("The current flight-controller settings do not confirm mandatory RID arming protection");
    if (uom > 0) {
        status +=
            tr(". Official live flight reporting is also required; registration and activation do not satisfy it");
    }
    return status + tr(". QGC does not change these parameters");
}
