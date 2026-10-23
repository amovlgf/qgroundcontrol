#pragma once

#include <QtCore/QHash>
#include <QtCore/QVariantList>
#include <QtQmlIntegration/QtQmlIntegration>

#include "Fact.h"
#include "management_protocol.h"

class RidParameterEntry final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RID parameters are discovered from the device")
    Q_PROPERTY(Fact* fact READ fact CONSTANT)
    Q_PROPERTY(bool writable READ writable CONSTANT)
    Q_PROPERTY(bool masked READ masked CONSTANT)
    Q_PROPERTY(bool action READ action CONSTANT)
    Q_PROPERTY(bool rebootRequired READ rebootRequired CONSTANT)
    Q_PROPERTY(bool unused READ unused CONSTANT)
public:
    explicit RidParameterEntry(const amov_device::Response& descriptor, QObject* parent = nullptr);

    Fact* fact() const { return _fact; }

    bool writable() const { return _descriptor.flags & amov_device::WRITABLE; }

    bool masked() const { return _descriptor.flags & amov_device::MASKED; }

    bool action() const { return _descriptor.flags & amov_device::ACTION; }

    bool rebootRequired() const { return _descriptor.flags & amov_device::REBOOT_REQUIRED; }

    bool unused() const { return _descriptor.flags & amov_device::UNUSED_IN_PROFILE; }

    quint16 index() const { return _descriptor.index; }

    amov_device::Type type() const { return _descriptor.type; }

    QString encodeValue(const QString& text, QByteArray& encoded) const;
    bool matchesDefault() const;
    bool updateValue(const amov_device::Response& response);
    static QVariant decodeValue(amov_device::Type type, const uint8_t* bytes, quint8 length);

private:
    amov_device::Response _descriptor;
    Fact* _fact;
};

class RidParameterManager final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    QVariantList entries() const { return _entries; }

    void clear();
    bool append(const amov_device::Response& descriptor);

    RidParameterEntry* find(const QString& name) const { return _byName.value(name, nullptr); }

    bool locked() const;
    bool defaultsMatch() const;

signals:
    void changed();

private:
    QVariantList _entries;
    QHash<QString, RidParameterEntry*> _byName;
};
