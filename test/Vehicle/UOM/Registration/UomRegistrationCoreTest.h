#pragma once

#include <QtCore/QObject>

class UomRegistrationCoreTest final : public QObject
{
    Q_OBJECT
private slots:
    void cryptoContract();
    void officialProfilesOnly();
    void parser_data();
    void parser();
    void durableRecoveryAndScopeIsolation();
    void schemaMigrationDoesNotInventAcknowledgements();
    void profileMigrationPreservesIdentityAndHistory();
    void encryptedWireAndPersistentLimit();
    void cancellationHasNoHiddenSerialAndNoLateResult();
    void memoryCredentialsAreExplicitAndRemovable();
    void unavailableKeyringRetainsMemory();
};
