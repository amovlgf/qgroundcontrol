/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "UnitTest.h"

class CodexAppServerClientTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _jsonlDecoderHandlesPartialAndMultipleLines_test();
    void _invalidJsonIsReported_test();
    void _requestsAreBlockedBeforeInitialize_test();
    void _requestResponseIdsAndErrors_test();
    void _requestTimeout_test();
    void _approvalRequestsAreDeclined_test();
    void _accountLoginAndModelSelection_test();
};
