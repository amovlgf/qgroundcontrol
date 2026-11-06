#pragma once

#include <QtCore/QObject>

class UomRegistrationWorkflowTest final : public QObject
{
    Q_OBJECT
private slots:
    void activationOrderAndStateSeparation();
    void localCooldownContinuesConfirmedActivation();
    void localCooldownConsentInvalidation_data();
    void localCooldownConsentInvalidation();
    void localCooldownRestartRequiresConfirmation();
    void localCooldownReadbackMustMatchConsent();
    void serverFailureDoesNotContinueActivation_data();
    void serverFailureDoesNotContinueActivation();
    void cancellationReconciliationRequiresConfirmation();
    void alreadyActiveStillRequiresDurableWrite();
    void unregisteredAircraftCannotActivate_data();
    void unregisteredAircraftCannotActivate();
    void initialBusinessRejectionDoesNotLockConfiguration();
    void uncertainDeviceWriteDoesNotReport();
    void activationReportRecoveryDoesNotRewriteDevice();
    void automaticRetryRequiresDurableProof_data();
    void automaticRetryRequiresDurableProof();
    void pendingCancellationCanRepairProducerProfile();
    void pendingCancellationCanRepairCancelUrl_data();
    void pendingCancellationCanRepairCancelUrl();
    void cancellationOrder();
    void inactiveReportRejectionChecksExistingDeregistration();
    void reportAcknowledgementSurvivesCompletionAndRestart();
    void reconciliationPreservesPreviousReportAcknowledgement();
    void cancellationTimeoutIsReconciled_data();
    void cancellationTimeoutIsReconciled();
    void restartWithoutProofRequiresExplicitWrite();
    void confirmedReportingRecovery_data();
    void confirmedReportingRecovery();
    void lostMemoryTokenRequiresExplicitRecovery();
    void lateVerificationCannotCrossDeviceContext_data();
    void lateVerificationCannotCrossDeviceContext();
    void lateDeviceWriteCannotCrossAircraft();
    void clearedIdentityRestoresOnlyHistory();
    void clearedIdentityPreservesPending_data();
    void clearedIdentityPreservesPending();
    void customerProfileRequiresValidEmbeddedKey_data();
    void customerProfileRequiresValidEmbeddedKey();
    void customerProfileUsesReadIdentity_data();
    void customerProfileUsesReadIdentity();
    void customerProfileRecoversWithoutKeyring();
    void customerProfileRejectsForeignPending();
};
