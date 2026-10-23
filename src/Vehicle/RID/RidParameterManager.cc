#include "RidParameterManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QtCore/QRegularExpression>

namespace {
FactMetaData::ValueType_t factType(amov_device::Type type)
{
    switch (type) {
        case amov_device::Type::UINT8:
            return FactMetaData::valueTypeUint8;
        case amov_device::Type::INT8:
            return FactMetaData::valueTypeInt8;
        case amov_device::Type::UINT32:
            return FactMetaData::valueTypeUint32;
        case amov_device::Type::FLOAT:
            return FactMetaData::valueTypeFloat;
        default:
            return FactMetaData::valueTypeString;
    }
}
}  // namespace

QVariant RidParameterEntry::decodeValue(amov_device::Type type, const uint8_t* bytes, quint8 length)
{
    switch (type) {
        case amov_device::Type::UINT8:
            return length == 1 ? QVariant(uint(bytes[0])) : QVariant();
        case amov_device::Type::INT8:
            return length == 1 ? QVariant(int(static_cast<qint8>(bytes[0]))) : QVariant();
        case amov_device::Type::UINT32:
            return length == 4 ? QVariant(amov_device::read_u32(bytes)) : QVariant();
        case amov_device::Type::FLOAT:
            if (length == 4 && std::isfinite(amov_device::read_float(bytes))) {
                return QVariant(amov_device::read_float(bytes));
            }
            return {};
        case amov_device::Type::CHAR20:
        case amov_device::Type::CHAR64: {
            const auto maximum = type == amov_device::Type::CHAR20 ? 20 : 64;
            if (length > maximum || std::memchr(bytes, 0, length)) {
                return {};
            }
            const QByteArray encoded(reinterpret_cast<const char*>(bytes), length);
            const QString value = QString::fromUtf8(encoded);
            return value.toUtf8() == encoded ? QVariant(value) : QVariant();
        }
        default:
            return {};
    }
}

RidParameterEntry::RidParameterEntry(const amov_device::Response& descriptor, QObject* parent)
    : QObject(parent)
    , _descriptor(descriptor)
    , _fact(new Fact(0, QString::fromLatin1(descriptor.name), factType(descriptor.type), this))
{
    auto* metadata = new FactMetaData(factType(descriptor.type), _fact->name(), _fact);
    metadata->setReadOnly(!writable());
    metadata->setMaxStringLength(descriptor.max_length);
    if (descriptor.type != amov_device::Type::CHAR20 && descriptor.type != amov_device::Type::CHAR64) {
        const auto type = factType(descriptor.type);
        const double minimum = std::max(double(descriptor.minimum), FactMetaData::minForType(type).toDouble());
        const double maximum = std::min(double(descriptor.maximum), FactMetaData::maxForType(type).toDouble());
        metadata->setRawMin(minimum);
        metadata->setRawMax(maximum);
    }
    if (descriptor.flags & amov_device::DEFAULT_AVAILABLE) {
        const auto value = decodeValue(descriptor.type, descriptor.default_value, descriptor.default_length);
        if (value.isValid()) {
            metadata->setRawDefaultValue(value);
        }
    }
    _fact->setMetaData(metadata);
    _fact->containerSetRawValue(decodeValue(descriptor.type, descriptor.value, descriptor.value_length));
}

QString RidParameterEntry::encodeValue(const QString& text, QByteArray& encoded) const
{
    encoded.clear();
    if (!writable() || action()) {
        return tr("This RID parameter cannot be edited");
    }
    if (masked() && text == QStringLiteral("********")) {
        return tr("Enter a new password; the hidden value cannot be saved");
    }
    const QString error = _fact->validate(text, false);
    if (!error.isEmpty()) {
        return error;
    }
    if (_descriptor.type == amov_device::Type::CHAR20 || _descriptor.type == amov_device::Type::CHAR64) {
        encoded = text.toUtf8();
        if (encoded.contains('\0') || encoded.size() < _descriptor.min_length ||
            encoded.size() > _descriptor.max_length || encoded.size() > 64) {
            encoded.clear();
            return tr("Value must contain %1 to %2 bytes without NUL characters")
                .arg(_descriptor.min_length)
                .arg(_descriptor.max_length);
        }
        if (_fact->name() == QStringLiteral("CN_WIFI_ID") && !text.isEmpty() &&
            !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{8}$")).match(text).hasMatch()) {
            encoded.clear();
            return tr("Wi-Fi identifier must contain exactly eight hexadecimal characters");
        }
        return {};
    }
    bool valid = false;
    const double value = text.toDouble(&valid);
    if (!valid || !std::isfinite(value) || value < _descriptor.minimum || value > _descriptor.maximum) {
        return tr("Value is outside the RID parameter range");
    }
    uint8_t bytes[4]{};
    if (_descriptor.type == amov_device::Type::FLOAT) {
        const float real = static_cast<float>(value);
        if (!std::isfinite(real)) {
            return tr("Value must be finite");
        }
        amov_device::write_float(bytes, real);
        encoded = QByteArray(reinterpret_cast<const char*>(bytes), 4);
    } else {
        if (std::trunc(value) != value) {
            return tr("An integer value is required");
        }
        if (_descriptor.type == amov_device::Type::UINT32) {
            if (value < 0 || value > 4294967295.0) {
                return tr("Value is outside the integer range");
            }
            amov_device::write_u32(bytes, static_cast<quint32>(value));
            encoded = QByteArray(reinterpret_cast<const char*>(bytes), 4);
        } else {
            const int lower = _descriptor.type == amov_device::Type::INT8 ? -128 : 0;
            const int upper = _descriptor.type == amov_device::Type::INT8 ? 127 : 255;
            if (value < lower || value > upper) {
                return tr("Value is outside the integer range");
            }
            encoded.append(static_cast<char>(static_cast<int>(value)));
        }
    }
    return {};
}

bool RidParameterEntry::updateValue(const amov_device::Response& response)
{
    if (response.type != _descriptor.type || QString::fromLatin1(response.name) != _fact->name()) {
        return false;
    }
    const auto value = decodeValue(response.type, response.value, response.value_length);
    if (!value.isValid()) {
        return false;
    }
    _fact->containerSetRawValue(value);
    return true;
}

void RidParameterManager::clear()
{
    const auto previous = _byName;
    _entries.clear();
    _byName.clear();
    emit changed();
    for (auto* entry : previous) {
        entry->deleteLater();
    }
}

bool RidParameterManager::append(const amov_device::Response& descriptor)
{
    const QString name = QString::fromLatin1(descriptor.name);
    const bool stringType =
        descriptor.type == amov_device::Type::CHAR20 || descriptor.type == amov_device::Type::CHAR64;
    const int capacity = descriptor.type == amov_device::Type::CHAR20 ? 20 : 64;
    if ((stringType && (descriptor.max_length > capacity || descriptor.value_length > descriptor.max_length)) ||
        (!stringType && (!std::isfinite(descriptor.minimum) || !std::isfinite(descriptor.maximum) ||
                         descriptor.minimum > descriptor.maximum)) ||
        ((descriptor.flags & amov_device::DEFAULT_AVAILABLE) &&
         !RidParameterEntry::decodeValue(descriptor.type, descriptor.default_value, descriptor.default_length)
              .isValid())) {
        return false;
    }
    if (name.isEmpty() || _byName.contains(name) || descriptor.type == amov_device::Type::NONE ||
        !RidParameterEntry::decodeValue(descriptor.type, descriptor.value, descriptor.value_length).isValid()) {
        return false;
    }
    auto* entry = new RidParameterEntry(descriptor, this);
    _byName.insert(name, entry);
    _entries.append(QVariant::fromValue(entry));
    emit changed();
    return true;
}

bool RidParameterManager::locked() const
{
    const auto* entry = find(QStringLiteral("LOCK_LEVEL"));
    return entry && entry->fact()->rawValue().toInt() > 0;
}

bool RidParameterEntry::matchesDefault() const
{
    if (!(_descriptor.flags & amov_device::DEFAULT_AVAILABLE) || masked() || action()) {
        return true;
    }
    return _fact->rawValue() == decodeValue(_descriptor.type, _descriptor.default_value, _descriptor.default_length);
}

bool RidParameterManager::defaultsMatch() const
{
    for (const auto* entry : _byName) {
        if (!entry->matchesDefault()) {
            return false;
        }
    }
    return true;
}
