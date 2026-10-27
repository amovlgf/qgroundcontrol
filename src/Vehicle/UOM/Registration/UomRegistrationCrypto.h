#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace UomRegistration {
[[nodiscard]] bool validKey(const QString& keyHex);
[[nodiscard]] std::optional<QByteArray> encrypt(const QByteArray& plaintext, const QString& keyHex);
[[nodiscard]] std::optional<QByteArray> decrypt(const QByteArray& cipherHex, const QString& keyHex);
}  // namespace UomRegistration
