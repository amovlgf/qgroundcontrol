#include "UomRegistrationProfile.h"

#ifdef QGC_B950_CUSTOMER_BUILD
#include "UomRegistrationProfileData.h"
#endif

bool UomRegistrationProfile::customerBuild()
{
#ifdef QGC_B950_CUSTOMER_BUILD
    return true;
#else
    return false;
#endif
}

UomRegistration::Config UomRegistrationProfile::config()
{
    UomRegistration::Config config;
#ifdef QGC_B950_CUSTOMER_BUILD
    config.environment = QStringLiteral("official");
    config.producerId = QString::fromLatin1(B950ProfileData::PRODUCER_ID);
    config.verifyUrl = QUrl(QString::fromLatin1(B950ProfileData::VERIFY_URL));
    config.reportUrl = QUrl(QString::fromLatin1(B950ProfileData::REPORT_URL));
    config.cancelUrl = QUrl(QString::fromLatin1(B950ProfileData::CANCEL_URL));
#endif
    return config;
}

QString UomRegistrationProfile::keyId()
{
#ifdef QGC_B950_CUSTOMER_BUILD
    return QString::fromLatin1(B950ProfileData::KEY_ID);
#else
    return {};
#endif
}
