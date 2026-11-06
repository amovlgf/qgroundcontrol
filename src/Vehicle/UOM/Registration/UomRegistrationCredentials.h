#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QThreadPool>

/// Linux Secret Service credentials, with explicit session-only storage for development profiles.
class UomRegistrationCredentials final : public QObject
{
    Q_OBJECT
public:
    explicit UomRegistrationCredentials(QObject* parent = nullptr, bool memoryOnly = false);
    ~UomRegistrationCredentials() override;
    void put(const QString& reference, const QString& value);
    void get(const QString& reference);
    void getPersistent(const QString& reference, bool forceRefresh = false);
    void remove(const QString& reference);

    [[nodiscard]] QString value(const QString& reference) const { return _values.value(reference); }

    [[nodiscard]] bool persistent(const QString& reference) const { return _persistent.contains(reference); }

signals:
    void completed(const QString& reference, bool success, bool persistent, const QString& detail);

private:
    friend class UomRegistrationCoreTest;
    friend class UomRegistrationWorkflowTest;

    void _run(const QString& reference, const QString& value, int operation);
    QHash<QString, QString> _values;
    QHash<QString, quint64> _generations;
    QSet<QString> _persistent;
    QThreadPool _worker;
    bool _memoryOnly;
};
