#pragma once

#include <QtCore/QObject>
#include <QtCore/QSet>

#ifdef QGC_UOM_DBUS
#include <QtDBus/QDBusUnixFileDescriptor>
#endif

class MultiVehicleManager;
class Vehicle;

/// Keeps Linux awake while a connected aircraft is armed; failures never alter flight readiness.
class UomSleepInhibitor final : public QObject
{
public:
    explicit UomSleepInhibitor(MultiVehicleManager* vehicles, QObject* parent = nullptr);

private:
    void _trackVehicle(Vehicle* vehicle);
    void _removeVehicle(Vehicle* vehicle);
    void _update();
    QSet<Vehicle*> _tracked;
    QSet<Vehicle*> _armed;
    bool _requestPending = false;
    bool _failureLogged = false;
#ifdef QGC_UOM_DBUS
    QDBusUnixFileDescriptor _inhibitFd;
#endif
};
