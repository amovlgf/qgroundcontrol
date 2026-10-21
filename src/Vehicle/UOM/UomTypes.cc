#include "UomTypes.h"

#include <cmath>

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QtEndian>

namespace {
bool printable(const QString& value, qsizetype maximum)
{
    if (value.isEmpty() || value.size() > maximum) {
        return false;
    }
    for (const QChar character : value) {
        if (character.unicode() < 0x21 || character.unicode() > 0x7e) {
            return false;
        }
    }
    return true;
}

bool coordinateValid(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude) && latitude >= -90 && latitude <= 90 &&
           longitude >= -180 && longitude <= 180;
}
}  // namespace

bool Uom::Identity::valid() const
{
    return aircraftId.size() == 20 && printable(aircraftId, 20) &&
           (registrationId.isEmpty() || (registrationId.size() >= 8 && printable(registrationId, 64) &&
                                         broadcastSuffix == registrationId.right(8))) &&
           printable(moduleId, 12) && revision != 0 && bootId != 0 && ridBootId != 0 && digest.size() == 32 &&
           digest != QByteArray(32, '\0') && aircraftClass <= 4 &&
           (broadcastSuffix.isEmpty() || (broadcastSuffix.size() == 8 && printable(broadcastSuffix, 8))) &&
           digest == computedDigest();
}

bool Uom::Identity::operator==(const Identity& other) const
{
    return aircraftId == other.aircraftId && registrationId == other.registrationId && moduleId == other.moduleId &&
           broadcastSuffix == other.broadcastSuffix && aircraftClass == other.aircraftClass &&
           revision == other.revision && bootId == other.bootId && ridBootId == other.ridBootId &&
           digest == other.digest && activated == other.activated;
}

QByteArray Uom::Identity::computedDigest() const
{
    if (moduleId.size() != 12) {
        return {};
    }
    for (QChar ch : moduleId) {
        if (!((ch >= u'0' && ch <= u'9') || (ch >= u'A' && ch <= u'F'))) {
            return {};
        }
    }
    QByteArray bytes("RID-ID-V1\0", 10);
    const quint32 littleEndianRevision = qToLittleEndian(revision);
    bytes.append(reinterpret_cast<const char*>(&littleEndianRevision), sizeof(littleEndianRevision));
    bytes.append(QByteArray::fromHex(moduleId.toLatin1()));
    bytes.append(static_cast<char>(aircraftClass));
    bytes.append(aircraftId.toLatin1());
    bytes.append(static_cast<char>(registrationId.size()));
    bytes.append(registrationId.toLatin1().leftJustified(64, '\0'));
    bytes.append(static_cast<char>(broadcastSuffix.size()));
    bytes.append(broadcastSuffix.toLatin1().leftJustified(8, '\0'));
    return bytes.size() == 115 ? QCryptographicHash::hash(bytes, QCryptographicHash::Blake2b_256) : QByteArray{};
}

QJsonObject Uom::Identity::toJson() const
{
    return {{"aircraft_id", aircraftId},
            {"registration_id", registrationId},
            {"module_id", moduleId},
            {"broadcast_suffix", broadcastSuffix},
            {"aircraft_class", aircraftClass},
            {"revision", qint64(revision)},
            {"boot_id", QString::number(bootId)},
            {"rid_boot_id", qint64(ridBootId)},
            {"digest", QString::fromLatin1(digest.toHex())}};
}

bool Uom::Telemetry::valid(quint64 nowUs) const
{
    return positionValid && operatorValid && receivedUs != 0 && nowUs >= receivedUs &&
           nowUs - receivedUs < DATA_TIMEOUT_US && utcMilliseconds >= 1577836800000LL &&
           coordinateValid(latitude, longitude) && coordinateValid(operatorLatitude, operatorLongitude) &&
           std::isfinite(altitudeMsl) && std::isfinite(relativeAltitude) && std::isfinite(velocityNorth) &&
           std::isfinite(velocityEast) && std::isfinite(velocityDown) && std::isfinite(operatorAltitudeEllipsoid);
}

QJsonObject Uom::Telemetry::toJson() const
{
    return {{"utc_ms", utcMilliseconds},
            {"latitude", latitude},
            {"longitude", longitude},
            {"altitude_msl_m", altitudeMsl},
            {"relative_altitude_m", relativeAltitude},
            {"velocity_ned_m_s", QJsonArray{velocityNorth, velocityEast, velocityDown}},
            {"operator_latitude", operatorLatitude},
            {"operator_longitude", operatorLongitude},
            {"operator_altitude_ellipsoid_m", operatorAltitudeEllipsoid},
            {"armed", armed}};
}
