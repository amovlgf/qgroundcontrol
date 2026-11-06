#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkRequest>

namespace UomReporting {

inline constexpr quint32 ALL_FIELDS = (1U << 21) - 1;
inline constexpr quint32 REQUIRED_FIELDS = ALL_FIELDS & ~((1U << 2) | (1U << 10) | (1U << 11) | (1U << 13));
inline constexpr qsizetype MAX_RESPONSE_BYTES = 16384;
inline constexpr quint64 MAX_TIMESTAMP_MS = (1ULL << 48) - 1;

enum class Mode
{
    Isolated,
    Commissioning,
    Official
};

struct Profile
{
    Mode mode = Mode::Isolated;
    QUrl endpoint;
    QString appId;
    QString uasModel;
    QString uscCode;
    QString platform;
    QString credentialReference;
    int source = 1;
    bool backfillValidated = false;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<Profile> fromJson(const QJsonObject& json);
};

/// Physical values in SI units; nullopt encodes the protocol's explicit unknown value.
struct Sample
{
    QString aircraftId;
    QString registrationId;
    quint32 fields = ALL_FIELDS;
    quint8 operationCategory = 0;
    quint8 aircraftClass = 0;
    quint8 operatorLocationType = 0;
    std::optional<double> operatorLongitude;
    std::optional<double> operatorLatitude;
    std::optional<double> operatorAltitudeEllipsoid;
    std::optional<double> longitude;
    std::optional<double> latitude;
    std::optional<double> trackDegrees;
    std::optional<double> groundSpeed;
    std::optional<double> relativeAltitude;
    std::optional<double> verticalSpeedUp;
    std::optional<double> altitudeEllipsoid;
    std::optional<double> pressureAltitude;
    quint8 operationState = 0;
    quint8 coordinateSystem = 0;
    quint8 horizontalAccuracy = 0;
    quint8 verticalAccuracy = 0;
    quint8 speedAccuracy = 0;
    quint64 timestampMs = 0;
    quint8 timestampAccuracy = 0;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<Sample> fromJson(const QJsonObject& json);
};

struct Result
{
    bool success = false;
    bool retryable = false;
    bool authenticationFailure = false;
    int httpStatus = 0;
    int code = 0;
    QString detail{};
};

[[nodiscard]] std::optional<QByteArray> encode(const Sample& sample, QString* error = nullptr);
[[nodiscard]] std::optional<Sample> decode(const QByteArray& packet, QString* error = nullptr);
[[nodiscard]] std::optional<QByteArray> signBody(const QByteArray& body, const QString& appKey);
[[nodiscard]] std::optional<QNetworkRequest> buildRequest(const Profile& profile, const QString& appKey,
                                                          const QString& orderId, const QByteArray& body);
[[nodiscard]] Result parseResponse(int httpStatus, const QByteArray& body);

}  // namespace UomReporting

Q_DECLARE_METATYPE(UomReporting::Result)
