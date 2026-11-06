#include "UomSleepInhibitor.h"

#ifdef QGC_UOM_DBUS
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#endif

#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

QGC_LOGGING_CATEGORY(UomSleepInhibitorLog, "qgc.uom.sleep")

UomSleepInhibitor::UomSleepInhibitor(MultiVehicleManager* vehicles, QObject* parent)
    : QObject(parent)
{
    if (!vehicles) {
        return;
    }
    connect(vehicles, &MultiVehicleManager::vehicleAdded, this, &UomSleepInhibitor::_trackVehicle);
    connect(vehicles, &MultiVehicleManager::vehicleRemoved, this, &UomSleepInhibitor::_removeVehicle);
    for (int index = 0; index < vehicles->vehicles()->count(); ++index) {
        _trackVehicle(qobject_cast<Vehicle*>(vehicles->vehicles()->get(index)));
    }
}

void UomSleepInhibitor::_trackVehicle(Vehicle* vehicle)
{
    if (!vehicle || _tracked.contains(vehicle)) {
        return;
    }
    _tracked.insert(vehicle);
    connect(vehicle, &Vehicle::armedChanged, this, [this, vehicle](bool armed) {
        if (armed) {
            _armed.insert(vehicle);
        } else {
            _armed.remove(vehicle);
        }
        _update();
    });
    connect(vehicle, &QObject::destroyed, this, [this, vehicle]() { _removeVehicle(vehicle); });
    if (vehicle->armed()) {
        _armed.insert(vehicle);
    }
    _update();
}

void UomSleepInhibitor::_removeVehicle(Vehicle* vehicle)
{
    if (!vehicle) {
        return;
    }
    disconnect(vehicle, nullptr, this, nullptr);
    _tracked.remove(vehicle);
    _armed.remove(vehicle);
    _update();
}

void UomSleepInhibitor::_update()
{
#ifdef QGC_UOM_DBUS
    if (_armed.isEmpty()) {
        _inhibitFd = QDBusUnixFileDescriptor();
        _failureLogged = false;
        return;
    }
    if (_requestPending || _inhibitFd.isValid()) {
        return;
    }
    _requestPending = true;
    QDBusMessage request = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("Inhibit"));
    request.setArguments({QStringLiteral("sleep"), QStringLiteral("QGroundControl"),
                          QStringLiteral("UOM reporting for an armed aircraft"), QStringLiteral("block")});
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(request, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
        watcher->deleteLater();
        _requestPending = false;
        if (_armed.isEmpty()) {
            return;
        }
        if (!reply.isError() && reply.value().isValid()) {
            _inhibitFd = reply.value();
            return;
        }
        if (!_failureLogged) {
            _failureLogged = true;
            qCWarning(UomSleepInhibitorLog) << "Linux sleep inhibition is unavailable; automatic reporting may pause";
        }
    });
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (!_armed.isEmpty() && !_failureLogged) {
        _failureLogged = true;
        qCWarning(UomSleepInhibitorLog) << "Qt DBus is unavailable; Linux sleep inhibition is disabled";
    } else if (_armed.isEmpty()) {
        _failureLogged = false;
    }
#endif
}
