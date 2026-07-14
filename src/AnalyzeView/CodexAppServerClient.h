/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariant>

#include <functional>

class CodexAppServerClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)

public:
    enum class State {
        Stopped,
        Starting,
        Initializing,
        SignedOut,
        Authorizing,
        SignedIn,
        Busy,
        Error,
    };
    Q_ENUM(State)

    using ResponseCallback = std::function<void(const QJsonValue &result, const QJsonObject &error)>;
    using WriteCallback = std::function<void(const QByteArray &line)>;

    explicit CodexAppServerClient(QObject *parent = nullptr);
    ~CodexAppServerClient() override;

    State state() const { return _state; }
    bool isInitialized() const { return _initialized; }
    bool isRunning() const;

    bool start();
    void stop();
    void setState(State state);

    static bool parseDeviceCodeLoginResponse(const QJsonObject &response, QString *loginId, QString *verificationUrl, QString *userCode);
    static bool parseChatGptAccountResponse(const QJsonObject &response, QString *email, QString *planType);
    static QVariantList parseModelListResponse(const QJsonObject &response);
    static int preferredModelIndex(const QVariantList &models, const QString &savedModel);

    qint64 request(const QString &method, const QJsonObject &params, ResponseCallback callback, int timeoutMsec = 30000);
    bool notify(const QString &method, const QJsonObject &params = QJsonObject());

    // Used by protocol tests to exercise the same decoder and request matcher
    // without starting a real Codex process.
    void feedStandardOutput(const QByteArray &data);
    void setTestTransport(const WriteCallback &callback);
    void setProtocolReadyForTesting(bool ready);

signals:
    void stateChanged();
    void initialized();
    void notificationReceived(const QString &method, const QJsonObject &params);
    void protocolError(const QString &errorText);
    void processError(const QString &errorText);
    void processExitedUnexpectedly();

private slots:
    void _processStarted();
    void _processErrorOccurred(QProcess::ProcessError error);
    void _processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void _readStandardOutput();
    void _readStandardError();
    void _checkRequestTimeouts();

private:
    struct PendingRequest {
        QString method;
        qint64 deadlineMsec = 0;
        ResponseCallback callback;
    };

    void _setState(State state);
    void _sendInitialize();
    qint64 _sendRequest(const QString &method, const QJsonObject &params, ResponseCallback callback, int timeoutMsec, bool allowBeforeInitialize);
    bool _writeLine(const QJsonObject &message);
    void _processLine(const QByteArray &line);
    void _handleMessage(const QJsonObject &message);
    void _handleResponse(const QJsonObject &message);
    void _handleServerRequest(const QJsonObject &message);
    void _sendServerResponse(const QJsonValue &id, const QJsonObject &result);
    void _failPendingRequests(const QString &errorText);
    QString _findCodexExecutable(QString *program, QStringList *arguments) const;
    bool _prepareCodexHome(QString *codexHome) const;

    QProcess *_process = nullptr;
    QTimer _requestTimeoutTimer;
    QHash<qint64, PendingRequest> _pendingRequests;
    QByteArray _standardOutputBuffer;
    WriteCallback _testTransport;
    State _state = State::Stopped;
    qint64 _nextRequestId = 1;
    bool _initialized = false;
    bool _stopping = false;
};
