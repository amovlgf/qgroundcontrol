#include "UomReportProtocol.h"

#include <array>
#include <cmath>
#include <limits>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QSslConfiguration>

namespace UomReporting {
namespace {
constexpr std::array<int, 21> FIELD_LENGTHS{20, 8, 1, 1, 1, 8, 2, 8, 2, 2, 2, 1, 2, 2, 1, 1, 1, 1, 1, 6, 1};

struct ScalarField
{
    const char* name;
    quint8 Sample::* member;
    quint8 maximum;
};

constexpr std::array<ScalarField, 9> SCALAR_FIELDS{{
    {"operationCategory", &Sample::operationCategory, 3},
    {"aircraftClass", &Sample::aircraftClass, 4},
    {"operatorLocationType", &Sample::operatorLocationType, 1},
    {"operationState", &Sample::operationState, 5},
    {"coordinateSystem", &Sample::coordinateSystem, 1},
    {"horizontalAccuracy", &Sample::horizontalAccuracy, 12},
    {"verticalAccuracy", &Sample::verticalAccuracy, 6},
    {"speedAccuracy", &Sample::speedAccuracy, 4},
    {"timestampAccuracy", &Sample::timestampAccuracy, 8},
}};

struct PhysicalField
{
    const char* name;
    std::optional<double> Sample::* member;
};

constexpr std::array<PhysicalField, 11> PHYSICAL_FIELDS{{
    {"operatorLongitude", &Sample::operatorLongitude},
    {"operatorLatitude", &Sample::operatorLatitude},
    {"operatorAltitudeEllipsoid", &Sample::operatorAltitudeEllipsoid},
    {"longitude", &Sample::longitude},
    {"latitude", &Sample::latitude},
    {"trackDegrees", &Sample::trackDegrees},
    {"groundSpeed", &Sample::groundSpeed},
    {"relativeAltitude", &Sample::relativeAltitude},
    {"verticalSpeedUp", &Sample::verticalSpeedUp},
    {"altitudeEllipsoid", &Sample::altitudeEllipsoid},
    {"pressureAltitude", &Sample::pressureAltitude},
}};

bool fail(QString* error, const QString& detail)
{
    if (error) {
        *error = detail;
    }
    return false;
}

bool printableAscii(const QString& text, int minimum, int maximum)
{
    if (text.size() < minimum || text.size() > maximum) {
        return false;
    }
    for (const QChar character : text) {
        if (character.unicode() < 0x21 || character.unicode() > 0x7e) {
            return false;
        }
    }
    return true;
}

bool headerValue(const QString& text)
{
    if (text.trimmed().isEmpty() || text.toUtf8().size() > 512) {
        return false;
    }
    for (const QChar character : text) {
        if (character.isNull() || character.category() == QChar::Other_Control) {
            return false;
        }
    }
    return true;
}

QByteArray littleEndian(quint64 value, int length)
{
    QByteArray bytes(length, '\0');
    for (int i = 0; i < length; ++i) {
        bytes[i] = char((value >> (i * 8)) & 0xff);
    }
    return bytes;
}

quint64 readLittle(const QByteArray& bytes, qsizetype offset, int length)
{
    quint64 result = 0;
    for (int i = 0; i < length; ++i) {
        result |= quint64(quint8(bytes[offset + i])) << (i * 8);
    }
    return result;
}

bool coordinate(const std::optional<double>& value, double limit, QByteArray& output)
{
    if (!value) {
        output += littleEndian(0xffffffff, 4);
        return true;
    }
    if (!std::isfinite(*value) || *value < -limit || *value > limit) {
        return false;
    }
    const qint64 scaled = std::llround(*value * 1e7);
    if (scaled == -1) {
        return false;  // 0xffffffff is reserved for an unavailable coordinate.
    }
    output += littleEndian(quint32(qint32(scaled)), 4);
    return true;
}

bool quantized(const std::optional<double>& value, double offset, double scale, quint64 minimum, quint64 maximum,
               quint64 unknown, int length, QByteArray& output)
{
    if (!value) {
        output = littleEndian(unknown, length);
        return true;
    }
    const double scaled = std::floor((*value + offset) * scale + 1e-9);
    if (!std::isfinite(*value) || !std::isfinite(scaled) || scaled < double(minimum) || scaled > double(maximum)) {
        return false;
    }
    output = littleEndian(quint64(scaled), length);
    return true;
}

std::optional<double> altitude(quint64 value, double offset)
{
    return value == 0 ? std::nullopt : std::optional<double>(double(value) * 0.5 - offset);
}

bool integer(const QJsonValue& value, quint64 maximum)
{
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= 0 && number <= double(maximum) &&
           std::floor(number) == number;
}

bool canonicalBody(const QByteArray& body)
{
    if (body.isEmpty() || body.size() % 2 != 0 || body.size() > 412) {
        return false;
    }
    for (const char character : body) {
        if (!(character >= '0' && character <= '9') && !(character >= 'A' && character <= 'F')) {
            return false;
        }
    }
    return true;
}
}  // namespace

bool Profile::valid() const
{
    if (!endpoint.isValid() || !endpoint.userInfo().isEmpty() || endpoint.hasQuery() || endpoint.hasFragment() ||
        !headerValue(appId) || !headerValue(uasModel) || !headerValue(uscCode) || !headerValue(platform) ||
        !headerValue(credentialReference) || source != 1) {
        return false;
    }
    if (mode == Mode::Isolated) {
        return (endpoint.scheme() == QStringLiteral("http") || endpoint.scheme() == QStringLiteral("https")) &&
               QHostAddress(endpoint.host()).isLoopback();
    }
    return (mode == Mode::Commissioning || mode == Mode::Official) &&
           endpoint == QUrl(QStringLiteral("https://uom.receive.caacic.cn/ridData"));
}

QJsonObject Profile::toJson() const
{
    return {{"schema", 1},
            {"mode", mode == Mode::Isolated        ? "isolated"
                     : mode == Mode::Commissioning ? "commissioning"
                                                   : "official"},
            {"endpoint", endpoint.toString(QUrl::FullyEncoded)},
            {"appId", appId},
            {"uasModel", uasModel},
            {"uscCode", uscCode},
            {"platform", platform},
            {"credentialReference", credentialReference},
            {"source", source},
            {"backfillValidated", backfillValidated}};
}

std::optional<Profile> Profile::fromJson(const QJsonObject& json)
{
    if (json.value("schema") != QJsonValue(1) || !json.value("source").isDouble() ||
        !json.value("backfillValidated").isBool()) {
        return std::nullopt;
    }
    Profile profile;
    const QString mode = json.value("mode").toString();
    if (mode != QStringLiteral("isolated") && mode != QStringLiteral("commissioning") &&
        mode != QStringLiteral("official")) {
        return std::nullopt;
    }
    profile.mode = mode == QStringLiteral("isolated")        ? Mode::Isolated
                   : mode == QStringLiteral("commissioning") ? Mode::Commissioning
                                                             : Mode::Official;
    profile.endpoint = QUrl(json.value("endpoint").toString(), QUrl::StrictMode);
    profile.appId = json.value("appId").toString();
    profile.uasModel = json.value("uasModel").toString();
    profile.uscCode = json.value("uscCode").toString();
    profile.platform = json.value("platform").toString();
    profile.credentialReference = json.value("credentialReference").toString();
    profile.source = json.value("source").toDouble() == 1.0 ? 1 : 0;
    profile.backfillValidated = json.value("backfillValidated").toBool();
    return profile.valid() ? std::optional<Profile>(profile) : std::nullopt;
}

QJsonObject Sample::toJson() const
{
    QJsonObject json{{"schema", 1},
                     {"aircraftId", aircraftId},
                     {"registrationId", registrationId},
                     {"fields", int(fields)},
                     {"timestampMs", QString::number(timestampMs)}};
    for (const auto& field : SCALAR_FIELDS) {
        json.insert(QString::fromLatin1(field.name), this->*field.member);
    }
    for (const auto& field : PHYSICAL_FIELDS) {
        const auto& value = this->*field.member;
        json.insert(QString::fromLatin1(field.name), value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null));
    }
    return json;
}

std::optional<Sample> Sample::fromJson(const QJsonObject& json)
{
    if (json.value("schema") != QJsonValue(1) || !integer(json.value("fields"), ALL_FIELDS) ||
        !json.value("aircraftId").isString() || !json.value("registrationId").isString()) {
        return std::nullopt;
    }
    Sample sample;
    sample.aircraftId = json.value("aircraftId").toString();
    sample.registrationId = json.value("registrationId").toString();
    sample.fields = quint32(json.value("fields").toInt());
    const QString timestamp = json.value("timestampMs").toString();
    bool validTimestamp = false;
    sample.timestampMs = timestamp.toULongLong(&validTimestamp);
    if (!validTimestamp || timestamp != QString::number(sample.timestampMs) || sample.timestampMs > MAX_TIMESTAMP_MS) {
        return std::nullopt;
    }
    for (const auto& field : SCALAR_FIELDS) {
        const auto value = json.value(QString::fromLatin1(field.name));
        if (!integer(value, field.maximum)) {
            return std::nullopt;
        }
        sample.*field.member = quint8(value.toInt());
    }
    for (const auto& field : PHYSICAL_FIELDS) {
        const auto value = json.value(QString::fromLatin1(field.name));
        if (!value.isNull() && (!value.isDouble() || !std::isfinite(value.toDouble()))) {
            return std::nullopt;
        }
        sample.*field.member = value.isNull() ? std::nullopt : std::optional<double>(value.toDouble());
    }
    return encode(sample) ? std::optional<Sample>(sample) : std::nullopt;
}

std::optional<QByteArray> encode(const Sample& sample, QString* error)
{
    if (error) {
        error->clear();
    }
    if ((sample.fields & REQUIRED_FIELDS) != REQUIRED_FIELDS || (sample.fields & ~ALL_FIELDS) != 0 ||
        !printableAscii(sample.aircraftId, 1, 20) ||
        (!sample.registrationId.isEmpty() && !printableAscii(sample.registrationId, 8, 8)) ||
        sample.timestampMs > MAX_TIMESTAMP_MS) {
        fail(error, QStringLiteral("Invalid UOM identity, field mask or timestamp"));
        return std::nullopt;
    }
    for (const auto& field : SCALAR_FIELDS) {
        if (sample.*field.member > field.maximum) {
            fail(error, QStringLiteral("Invalid UOM category or accuracy"));
            return std::nullopt;
        }
    }
    std::array<QByteArray, 21> items;
    items[0] = QByteArray(20 - sample.aircraftId.size(), '\0') + sample.aircraftId.toLatin1();
    items[1] = sample.registrationId.isEmpty() ? QByteArray(8, '\0') : sample.registrationId.toLatin1();
    items[2] = littleEndian(sample.operationCategory, 1);
    items[3] = littleEndian(sample.aircraftClass, 1);
    items[4] = littleEndian(sample.operatorLocationType, 1);
    const bool validCoordinates =
        coordinate(sample.operatorLongitude, 180, items[5]) && coordinate(sample.operatorLatitude, 90, items[5]) &&
        coordinate(sample.longitude, 180, items[7]) && coordinate(sample.latitude, 90, items[7]);
    const bool validValues = quantized(sample.operatorAltitudeEllipsoid, 1000, 2, 1, 65535, 0, 2, items[6]) &&
                             quantized(sample.trackDegrees, 0, 10, 0, 3599, 65535, 2, items[8]) &&
                             quantized(sample.groundSpeed, 0, 10, 0, 65534, 65535, 2, items[9]) &&
                             quantized(sample.relativeAltitude, 9000, 2, 1, 65535, 0, 2, items[10]) &&
                             quantized(sample.altitudeEllipsoid, 1000, 2, 1, 65535, 0, 2, items[12]) &&
                             quantized(sample.pressureAltitude, 1000, 2, 1, 65535, 0, 2, items[13]);
    if (!validCoordinates || !validValues) {
        fail(error, QStringLiteral("UOM position, height or speed is outside its encoded range"));
        return std::nullopt;
    }
    quint8 vertical = 0xff;
    if (sample.verticalSpeedUp) {
        const double value = *sample.verticalSpeedUp;
        const double magnitude = std::floor(std::abs(value) * 2 + 1e-9);
        if (!std::isfinite(value) || magnitude > 127 || (value < 0 && magnitude >= 127)) {
            fail(error, QStringLiteral("UOM vertical speed is outside its encoded range"));
            return std::nullopt;
        }
        vertical = quint8(magnitude) | (value < 0 ? 0x80 : 0);
    }
    items[11] = littleEndian(vertical, 1);
    items[14] = littleEndian(sample.operationState, 1);
    items[15] = littleEndian(sample.coordinateSystem, 1);
    items[16] = littleEndian(sample.horizontalAccuracy, 1);
    items[17] = littleEndian(sample.verticalAccuracy, 1);
    items[18] = littleEndian(sample.speedAccuracy, 1);
    items[19] = littleEndian(sample.timestampMs, 6);
    items[20] = littleEndian(sample.timestampAccuracy, 1);
    QByteArray flags(3, '\0');
    QByteArray content;
    for (int i = 0; i < 21; ++i) {
        if (sample.fields & (1U << i)) {
            flags[i / 7] = char(quint8(flags[i / 7]) | (0x80 >> (i % 7)));
            content += items[i];
        }
    }
    flags[0] = char(quint8(flags[0]) | 1);
    flags[1] = char(quint8(flags[1]) | 1);
    return QByteArray::fromHex("FF20") + char(content.size()) + flags + content;
}

std::optional<Sample> decode(const QByteArray& packet, QString* error)
{
    if (error) {
        error->clear();
    }
    if (packet.size() < 6 || quint8(packet[0]) != 0xff || quint8(packet[1]) != 0x20 || quint8(packet[2]) == 0 ||
        quint8(packet[2]) > 200 || packet.size() != 6 + quint8(packet[2]) || !(quint8(packet[3]) & 1) ||
        !(quint8(packet[4]) & 1) || (quint8(packet[5]) & 1)) {
        fail(error, QStringLiteral("Invalid UOM packet header or length"));
        return std::nullopt;
    }
    Sample sample;
    sample.fields = 0;
    std::array<qsizetype, 21> offsets;
    offsets.fill(-1);
    qsizetype cursor = 6;
    for (int i = 0; i < 21; ++i) {
        if (quint8(packet[3 + i / 7]) & (0x80 >> (i % 7))) {
            sample.fields |= 1U << i;
            offsets[i] = cursor;
            cursor += FIELD_LENGTHS[i];
        }
    }
    if (cursor != packet.size() || (sample.fields & REQUIRED_FIELDS) != REQUIRED_FIELDS) {
        fail(error, QStringLiteral("Invalid UOM field flags or content length"));
        return std::nullopt;
    }
    auto aircraft = packet.mid(offsets[0], 20);
    while (aircraft.startsWith('\0')) {
        aircraft.remove(0, 1);
    }
    sample.aircraftId = QString::fromLatin1(aircraft);
    const auto registration = packet.mid(offsets[1], 8);
    sample.registrationId = registration == QByteArray(8, '\0') ? QString() : QString::fromLatin1(registration);
    const auto read = [&](int i) {
        return offsets[i] < 0 ? quint64(0) : readLittle(packet, offsets[i], FIELD_LENGTHS[i]);
    };
    const auto position = [&](int index, int delta) -> std::optional<double> {
        const auto value = quint32(readLittle(packet, offsets[index] + delta, 4));
        return value == 0xffffffff ? std::nullopt : std::optional<double>(double(qint32(value)) / 1e7);
    };
    sample.operationCategory = quint8(read(2));
    sample.aircraftClass = quint8(read(3));
    sample.operatorLocationType = quint8(read(4));
    sample.operatorLongitude = position(5, 0);
    sample.operatorLatitude = position(5, 4);
    sample.operatorAltitudeEllipsoid = altitude(read(6), 1000);
    sample.longitude = position(7, 0);
    sample.latitude = position(7, 4);
    sample.trackDegrees = read(8) == 65535 ? std::nullopt : std::optional<double>(double(read(8)) / 10);
    sample.groundSpeed = read(9) == 65535 ? std::nullopt : std::optional<double>(double(read(9)) / 10);
    sample.relativeAltitude = altitude(read(10), 9000);
    if (offsets[11] >= 0 && read(11) != 255) {
        sample.verticalSpeedUp = double(read(11) & 127) * ((read(11) & 128) ? -0.5 : 0.5);
    }
    sample.altitudeEllipsoid = altitude(read(12), 1000);
    sample.pressureAltitude = altitude(read(13), 1000);
    sample.operationState = quint8(read(14));
    sample.coordinateSystem = quint8(read(15));
    sample.horizontalAccuracy = quint8(read(16));
    sample.verticalAccuracy = quint8(read(17));
    sample.speedAccuracy = quint8(read(18));
    sample.timestampMs = read(19);
    sample.timestampAccuracy = quint8(read(20));
    if (!encode(sample, error)) {
        return std::nullopt;
    }
    return sample;
}

std::optional<QByteArray> signBody(const QByteArray& body, const QString& appKey)
{
    if (!canonicalBody(body) || appKey.isEmpty() || appKey.size() > 4096) {
        return std::nullopt;
    }
    QByteArray key = appKey.toUtf8();
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned int length = 0;
    const auto result =
        HMAC(EVP_sm3(), key.constData(), int(key.size()), reinterpret_cast<const unsigned char*>(body.constData()),
             size_t(body.size()), output.data(), &length);
    OPENSSL_cleanse(key.data(), size_t(key.size()));
    if (!result || length != 32) {
        return std::nullopt;
    }
    return QByteArray(reinterpret_cast<const char*>(output.data()), length).toHex();
}

std::optional<QNetworkRequest> buildRequest(const Profile& profile, const QString& appKey, const QString& orderId,
                                            const QByteArray& body)
{
    static const QRegularExpression ORDER_ID(QStringLiteral("^[0-9]{8}-[A-Za-z0-9]{8}$"));
    if (!profile.valid() || !ORDER_ID.match(orderId).hasMatch() || !canonicalBody(body) ||
        !decode(QByteArray::fromHex(body))) {
        return std::nullopt;
    }
    const auto signature = signBody(body, appKey);
    if (!signature) {
        return std::nullopt;
    }
    QNetworkRequest request(profile.endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    request.setRawHeader("appId", profile.appId.toUtf8());
    request.setRawHeader("sign", *signature);
    request.setRawHeader("source", QByteArray::number(profile.source));
    request.setRawHeader("orderId", orderId.toLatin1());
    request.setRawHeader("uasModel", profile.uasModel.toUtf8());
    request.setRawHeader("uscCode", profile.uscCode.toUtf8());
    request.setRawHeader("platform", profile.platform.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(ssl);
    return request;
}

Result parseResponse(int httpStatus, const QByteArray& body)
{
    Result result;
    result.httpStatus = httpStatus;
    if (httpStatus != 200) {
        result.retryable = httpStatus == 0 || httpStatus == 408 || httpStatus == 429 || httpStatus >= 500;
        result.authenticationFailure = httpStatus == 401 || httpStatus == 403;
        result.detail = QStringLiteral("UOM returned HTTP status %1").arg(httpStatus);
        return result;
    }
    QJsonParseError error;
    const QJsonDocument document =
        body.size() <= MAX_RESPONSE_BYTES ? QJsonDocument::fromJson(body, &error) : QJsonDocument();
    if (body.size() > MAX_RESPONSE_BYTES || error.error != QJsonParseError::NoError || !document.isObject()) {
        result.detail = QStringLiteral("UOM returned an invalid response");
        return result;
    }
    const auto object = document.object();
    const auto code = object.value("code");
    if (integer(code, 999)) {
        result.code = code.toInt();
    } else if (code.isString()) {
        bool valid = false;
        result.code = code.toString().toInt(&valid);
        if (!valid || code.toString() != QString::number(result.code) || result.code < 100 || result.code > 999) {
            result.code = 0;
        }
    }
    if (result.code < 100 || !object.value("msg").isString()) {
        result.detail = QStringLiteral("UOM returned an invalid response code or message");
        return result;
    }
    result.success = result.code == 200;
    result.authenticationFailure = result.code == 401 || result.code == 403;
    result.retryable = result.code == 408 || result.code == 429 || result.code >= 500;
    // Server messages can echo submitted identities or credentials; keep diagnostics local and bounded.
    result.detail = result.success ? QStringLiteral("UOM accepted the report")
                                   : QStringLiteral("UOM rejected the report (code %1)").arg(result.code);
    return result;
}

}  // namespace UomReporting
