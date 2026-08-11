/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "CodexAppServerClient.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStandardPaths>

#include <utility>

namespace {

constexpr int kRequestTimerIntervalMsec = 250;
constexpr int kDefaultRequestTimeoutMsec = 30000;

QJsonObject protocolErrorObject(const QString &message)
{
    return QJsonObject{
        { QStringLiteral("code"), -1 },
        { QStringLiteral("message"), message }
    };
}

} // namespace

CodexAppServerClient::CodexAppServerClient(QObject *parent)
    : QObject(parent)
    , _process(new QProcess(this))
{
    _requestTimeoutTimer.setInterval(kRequestTimerIntervalMsec);
    (void) connect(&_requestTimeoutTimer, &QTimer::timeout, this, &CodexAppServerClient::_checkRequestTimeouts);
    (void) connect(_process, &QProcess::started, this, &CodexAppServerClient::_processStarted);
    (void) connect(_process, &QProcess::errorOccurred, this, &CodexAppServerClient::_processErrorOccurred);
    (void) connect(_process, &QProcess::finished, this, &CodexAppServerClient::_processFinished);
    (void) connect(_process, &QProcess::readyReadStandardOutput, this, &CodexAppServerClient::_readStandardOutput);
    (void) connect(_process, &QProcess::readyReadStandardError, this, &CodexAppServerClient::_readStandardError);
}

CodexAppServerClient::~CodexAppServerClient()
{
    stop();
}

bool CodexAppServerClient::isRunning() const
{
    return _testTransport || (_process && _process->state() != QProcess::NotRunning);
}

void CodexAppServerClient::_setState(State state)
{
    if (_state == state) {
        return;
    }

    _state = state;
    emit stateChanged();
}

void CodexAppServerClient::setState(State state)
{
    _setState(state);
}

bool CodexAppServerClient::parseDeviceCodeLoginResponse(const QJsonObject &response, QString *loginId, QString *verificationUrl, QString *userCode)
{
    const QString responseLoginId = response.value(QStringLiteral("loginId")).toString();
    const QString responseVerificationUrl = response.value(QStringLiteral("verificationUrl")).toString();
    const QString responseUserCode = response.value(QStringLiteral("userCode")).toString();
    if (responseLoginId.isEmpty() || responseVerificationUrl.isEmpty() || responseUserCode.isEmpty()) {
        return false;
    }

    if (loginId) {
        *loginId = responseLoginId;
    }
    if (verificationUrl) {
        *verificationUrl = responseVerificationUrl;
    }
    if (userCode) {
        *userCode = responseUserCode;
    }
    return true;
}

bool CodexAppServerClient::parseChatGptAccountResponse(const QJsonObject &response, QString *email, QString *planType)
{
    const QJsonObject account = response.value(QStringLiteral("account")).toObject();
    if (account.value(QStringLiteral("type")).toString() != QStringLiteral("chatgpt")) {
        return false;
    }

    if (email) {
        *email = account.value(QStringLiteral("email")).toString();
    }
    if (planType) {
        *planType = account.value(QStringLiteral("planType")).toString();
    }
    return true;
}

QVariantList CodexAppServerClient::parseModelListResponse(const QJsonObject &response)
{
    QVariantList models;
    for (const QJsonValue &value : response.value(QStringLiteral("data")).toArray()) {
        const QJsonObject model = value.toObject();
        const QString modelId = model.value(QStringLiteral("model")).toString();
        const QString displayName = model.value(QStringLiteral("displayName")).toString();
        if (modelId.isEmpty() || displayName.isEmpty()) {
            continue;
        }
        models.append(QVariantMap{
            { QStringLiteral("model"), modelId },
            { QStringLiteral("displayName"), displayName },
            { QStringLiteral("isDefault"), model.value(QStringLiteral("isDefault")).toBool() }
        });
    }
    return models;
}

int CodexAppServerClient::preferredModelIndex(const QVariantList &models, const QString &savedModel)
{
    if (!savedModel.isEmpty()) {
        for (int i = 0; i < models.size(); ++i) {
            if (models.at(i).toMap().value(QStringLiteral("model")).toString() == savedModel) {
                return i;
            }
        }
    }
    for (int i = 0; i < models.size(); ++i) {
        if (models.at(i).toMap().value(QStringLiteral("isDefault")).toBool()) {
            return i;
        }
    }
    return models.isEmpty() ? -1 : 0;
}

QString CodexAppServerClient::_findCodexExecutable(QString *program, QStringList *arguments) const
{
    QString executable;
#ifdef Q_OS_WIN
    const QStringList candidates{ QStringLiteral("codex.exe"), QStringLiteral("codex.cmd"), QStringLiteral("codex") };
#else
    const QStringList candidates{ QStringLiteral("codex") };
#endif

    for (const QString &candidate : candidates) {
        executable = QStandardPaths::findExecutable(candidate);
        if (!executable.isEmpty()) {
            break;
        }
    }

    if (executable.isEmpty()) {
        return QString();
    }

    if (program && arguments) {
#ifdef Q_OS_WIN
        if (executable.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive)) {
            const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            *program = environment.value(QStringLiteral("ComSpec"), QStringLiteral("cmd.exe"));
            *arguments = QStringList{ QStringLiteral("/C"), executable, QStringLiteral("app-server"), QStringLiteral("--listen"), QStringLiteral("stdio://") };
        } else {
            *program = executable;
            *arguments = QStringList{ QStringLiteral("app-server"), QStringLiteral("--listen"), QStringLiteral("stdio://") };
        }
#else
        *program = executable;
        *arguments = QStringList{ QStringLiteral("app-server"), QStringLiteral("--listen"), QStringLiteral("stdio://") };
#endif
    }

    return executable;
}

bool CodexAppServerClient::_prepareCodexHome(QString *codexHome) const
{
    const QString appDataLocation = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appDataLocation.isEmpty()) {
        return false;
    }

    const QString applicationCodexHome = QDir(appDataLocation).filePath(QStringLiteral("codex"));
    if (!QDir().mkpath(applicationCodexHome)) {
        return false;
    }

    if (codexHome) {
        *codexHome = applicationCodexHome;
    }
    return true;
}

bool CodexAppServerClient::start()
{
    if (_testTransport || (_process && _process->state() != QProcess::NotRunning)) {
        return true;
    }

    QString program;
    QStringList arguments;
    if (_findCodexExecutable(&program, &arguments).isEmpty()) {
        _setState(State::Error);
        emit processError(tr("Codex CLI was not found. Install or update Codex CLI and try again."));
        return false;
    }

    QString codexHome;
    if (!_prepareCodexHome(&codexHome)) {
        _setState(State::Error);
        emit processError(tr("Unable to create the application-specific Codex home directory."));
        return false;
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("CODEX_HOME"), codexHome);
    _process->setProcessEnvironment(environment);
    _process->setProgram(program);
    _process->setArguments(arguments);
    _standardOutputBuffer.clear();
    _initialized = false;
    _stopping = false;
    _setState(State::Starting);
    _process->start();
    _requestTimeoutTimer.start();
    return true;
}

void CodexAppServerClient::stop()
{
    _requestTimeoutTimer.stop();
    _initialized = false;
    _failPendingRequests(tr("Codex authentication service stopped."));

    if (_process && _process->state() != QProcess::NotRunning) {
        _stopping = true;
        _process->terminate();
        if (!_process->waitForFinished(500)) {
            _process->kill();
        }
    }

    _standardOutputBuffer.clear();
    _setState(State::Stopped);
}

void CodexAppServerClient::_processStarted()
{
    _setState(State::Initializing);
    _sendInitialize();
}

void CodexAppServerClient::_sendInitialize()
{
    const QJsonObject params{
        { QStringLiteral("clientInfo"), QJsonObject{
            { QStringLiteral("name"), QStringLiteral("px4_ai_flight_diagnostics") },
            { QStringLiteral("title"), QStringLiteral("PX4 AI Flight Diagnostics") },
            { QStringLiteral("version"), QCoreApplication::applicationVersion() }
        } }
    };

    (void) _sendRequest(QStringLiteral("initialize"), params, [this](const QJsonValue &, const QJsonObject &error) {
        if (!error.isEmpty()) {
            _setState(State::Error);
            emit processError(tr("Codex App Server initialization failed: %1").arg(error.value(QStringLiteral("message")).toString()));
            return;
        }

        if (!_writeLine(QJsonObject{ { QStringLiteral("method"), QStringLiteral("initialized") }, { QStringLiteral("params"), QJsonObject() } })) {
            return;
        }

        _initialized = true;
        _setState(State::SignedOut);
        emit initialized();
    }, kDefaultRequestTimeoutMsec, true);
}

qint64 CodexAppServerClient::request(const QString &method, const QJsonObject &params, ResponseCallback callback, int timeoutMsec)
{
    return _sendRequest(method, params, std::move(callback), timeoutMsec, false);
}

qint64 CodexAppServerClient::_sendRequest(const QString &method, const QJsonObject &params, ResponseCallback callback, int timeoutMsec, bool allowBeforeInitialize)
{
    if (method.trimmed().isEmpty() || (!_initialized && !allowBeforeInitialize)) {
        return -1;
    }

    const qint64 id = _nextRequestId++;
    const QJsonObject message{
        { QStringLiteral("method"), method },
        { QStringLiteral("id"), id },
        { QStringLiteral("params"), params }
    };
    if (!_writeLine(message)) {
        return -1;
    }

    PendingRequest pending;
    pending.method = method;
    pending.deadlineMsec = QDateTime::currentMSecsSinceEpoch() + qMax(1, timeoutMsec);
    pending.callback = std::move(callback);
    _pendingRequests.insert(id, std::move(pending));
    return id;
}

bool CodexAppServerClient::notify(const QString &method, const QJsonObject &params)
{
    if (method.trimmed().isEmpty() || (!_initialized && method != QStringLiteral("initialized"))) {
        return false;
    }

    return _writeLine(QJsonObject{
        { QStringLiteral("method"), method },
        { QStringLiteral("params"), params }
    });
}

bool CodexAppServerClient::_writeLine(const QJsonObject &message)
{
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');

    if (_testTransport) {
        _testTransport(line);
        return true;
    }
    if (!_process || _process->state() != QProcess::Running) {
        return false;
    }

    qint64 written = 0;
    while (written < line.size()) {
        const qint64 bytesWritten = _process->write(line.constData() + written, line.size() - written);
        if (bytesWritten <= 0) {
            return false;
        }
        written += bytesWritten;
    }
    return true;
}

void CodexAppServerClient::_readStandardOutput()
{
    if (_process) {
        feedStandardOutput(_process->readAllStandardOutput());
    }
}

void CodexAppServerClient::_readStandardError()
{
    if (_process) {
        // Keep stderr diagnostic-only. In particular, never expose or persist its contents.
        (void) _process->readAllStandardError();
    }
}

void CodexAppServerClient::feedStandardOutput(const QByteArray &data)
{
    _standardOutputBuffer.append(data);
    while (true) {
        const qsizetype newlineIndex = _standardOutputBuffer.indexOf('\n');
        if (newlineIndex < 0) {
            break;
        }

        const QByteArray line = _standardOutputBuffer.left(newlineIndex).trimmed();
        _standardOutputBuffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) {
            _processLine(line);
        }
    }
}

void CodexAppServerClient::_processLine(const QByteArray &line)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit protocolError(tr("Codex App Server returned invalid JSON."));
        return;
    }

    _handleMessage(document.object());
}

void CodexAppServerClient::_handleMessage(const QJsonObject &message)
{
    if (message.contains(QStringLiteral("id")) && message.contains(QStringLiteral("method"))) {
        _handleServerRequest(message);
        return;
    }
    if (message.contains(QStringLiteral("id"))) {
        _handleResponse(message);
        return;
    }

    const QString method = message.value(QStringLiteral("method")).toString();
    if (!method.isEmpty()) {
        emit notificationReceived(method, message.value(QStringLiteral("params")).toObject());
    }
}

void CodexAppServerClient::_handleResponse(const QJsonObject &message)
{
    const QJsonValue idValue = message.value(QStringLiteral("id"));
    if (!idValue.isDouble()) {
        return;
    }

    const qint64 id = static_cast<qint64>(idValue.toDouble());
    auto pendingIterator = _pendingRequests.find(id);
    if (pendingIterator == _pendingRequests.end()) {
        return;
    }

    PendingRequest pending = std::move(pendingIterator.value());
    _pendingRequests.erase(pendingIterator);
    if (pending.callback) {
        pending.callback(message.value(QStringLiteral("result")), message.value(QStringLiteral("error")).toObject());
    }
}

void CodexAppServerClient::_handleServerRequest(const QJsonObject &message)
{
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QString method = message.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("item/commandExecution/requestApproval")
            || method == QStringLiteral("item/fileChange/requestApproval")) {
        _sendServerResponse(id, QJsonObject{ { QStringLiteral("decision"), QStringLiteral("decline") } });
    } else if (method == QStringLiteral("item/permissions/requestApproval")) {
        _sendServerResponse(id, QJsonObject{
            { QStringLiteral("permissions"), QJsonObject{
                { QStringLiteral("fileSystem"), QJsonValue() },
                { QStringLiteral("network"), QJsonValue() }
            } },
            { QStringLiteral("scope"), QStringLiteral("turn") }
        });
    } else if (method == QStringLiteral("item/mcpServerElicitation/request")) {
        _sendServerResponse(id, QJsonObject{ { QStringLiteral("action"), QStringLiteral("decline") } });
    } else {
        _sendServerResponse(id, QJsonObject{ { QStringLiteral("decision"), QStringLiteral("decline") } });
    }
}

void CodexAppServerClient::_sendServerResponse(const QJsonValue &id, const QJsonObject &result)
{
    (void) _writeLine(QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("result"), result }
    });
}

void CodexAppServerClient::_checkRequestTimeouts()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<qint64> expiredIds;
    for (auto iterator = _pendingRequests.cbegin(); iterator != _pendingRequests.cend(); ++iterator) {
        if (iterator.value().deadlineMsec <= now) {
            expiredIds.append(iterator.key());
        }
    }

    for (const qint64 id : expiredIds) {
        auto iterator = _pendingRequests.find(id);
        if (iterator == _pendingRequests.end()) {
            continue;
        }

        PendingRequest pending = std::move(iterator.value());
        _pendingRequests.erase(iterator);
        if (pending.callback) {
            pending.callback(QJsonValue(), protocolErrorObject(tr("Codex App Server request timed out.")));
        }
    }
}

void CodexAppServerClient::_failPendingRequests(const QString &errorText)
{
    const QJsonObject error = protocolErrorObject(errorText);
    const auto pendingRequests = std::exchange(_pendingRequests, {});
    for (const PendingRequest &pending : pendingRequests) {
        if (pending.callback) {
            pending.callback(QJsonValue(), error);
        }
    }
}

void CodexAppServerClient::_processErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        _setState(State::Error);
        emit processError(tr("Codex CLI was not found. Install or update Codex CLI and try again."));
    }
}

void CodexAppServerClient::_processFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const bool expected = _stopping;
    _stopping = false;
    _initialized = false;
    _requestTimeoutTimer.stop();
    _failPendingRequests(tr("Authentication service exited unexpectedly."));
    _standardOutputBuffer.clear();

    if (!expected && exitStatus == QProcess::CrashExit) {
        _setState(State::Error);
        emit processError(tr("Authentication service exited unexpectedly."));
        emit processExitedUnexpectedly();
    } else if (!expected && exitCode != 0) {
        _setState(State::Error);
        emit processError(tr("Authentication service exited unexpectedly."));
        emit processExitedUnexpectedly();
    } else {
        _setState(State::Stopped);
    }
}

void CodexAppServerClient::setTestTransport(const WriteCallback &callback)
{
    _testTransport = callback;
}

void CodexAppServerClient::setProtocolReadyForTesting(bool ready)
{
    _initialized = ready;
    if (ready) {
        _requestTimeoutTimer.start();
    } else {
        _requestTimeoutTimer.stop();
    }
    _setState(ready ? State::SignedOut : State::Stopped);
}
