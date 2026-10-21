#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace Uom {

enum class Environment : quint8
{
    Unconfigured,
    Mock,
    Official
};
enum class State : quint8
{
    NotConfigured,
    WaitingIdentity,
    WaitingData,
    Authenticating,
    Reporting,
    Recovering,
    Fault
};
enum class Fault : quint16
{
    None,
    Identity,
    Activation,
    Telemetry,
    Authentication,
    Network,
    Response,
    Timeout
};

struct Identity
{
    QString aircraftId;
    QString registrationId;
    QString moduleId;
    QString broadcastSuffix;
    quint8 aircraftClass = 0;
    quint32 revision = 0;
    quint64 bootId = 0;
    quint32 ridBootId = 0;
    QByteArray digest;
    bool activated = false;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] QByteArray computedDigest() const;
    [[nodiscard]] QJsonObject toJson() const;
    bool operator==(const Identity& other) const;
};

struct Telemetry
{
    qint64 utcMilliseconds = 0;
    quint64 receivedUs = 0;
    double latitude = 0;
    double longitude = 0;
    double altitudeMsl = 0;
    double relativeAltitude = 0;
    double velocityNorth = 0;
    double velocityEast = 0;
    double velocityDown = 0;
    double operatorLatitude = 0;
    double operatorLongitude = 0;
    double operatorAltitudeEllipsoid = 0;
    bool positionValid = false;
    bool operatorValid = false;
    bool armed = false;

    [[nodiscard]] bool valid(quint64 nowUs) const;
    [[nodiscard]] QJsonObject toJson() const;
};

inline constexpr quint64 DATA_TIMEOUT_US = 3000000;
inline constexpr quint64 ACK_TIMEOUT_US = 5000000;
inline constexpr quint64 REPORT_INTERVAL_US = 1000000;

}  // namespace Uom
