#pragma once

#include <QtCore/QObject>

class UomRegistrationWorkflowTest final : public QObject
{
    Q_OBJECT
private slots:
    void activationOrderAndStateSeparation();
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
};
