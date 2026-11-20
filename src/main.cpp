#include "app.h"
#include "autostart.h"
#include "selfcheck.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QLockFile>
#include <QSettings>
#include <QTimer>
#include <QTranslator>

#include <windows.h>

namespace {

// Per-user channel through which a second launch hands its request to the running instance.
QString instanceChannel()
{
    return QStringLiteral("jieying-") + qEnvironmentVariable("USERNAME");
}

bool forwardToRunningInstance(const QByteArray &message)
{
    QLocalSocket socket;
    socket.connectToServer(instanceChannel());
    if (!socket.waitForConnected(1000))
        return false;
    socket.write(message + '\n');
    return socket.waitForBytesWritten(1000);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Jieying");
    QApplication::setApplicationName("Jieying");
    QApplication::setApplicationDisplayName(QStringLiteral("截影"));
    QApplication::setApplicationVersion(QStringLiteral(JIEYING_VERSION));
    QApplication::setQuitOnLastWindowClosed(false); // lives in the tray

    // Portable copy: a "jieying.portable" file next to the exe keeps settings in data\ beside it
    // instead of the registry. Decided before anything reads settings.
    const QDir appDir(QCoreApplication::applicationDirPath());
    if (QFile::exists(appDir.filePath("jieying.portable"))) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, appDir.filePath("data"));
    }

    // Qt's own strings (file dialogs, text field menus, key names) in Chinese. windeployqt puts a
    // merged qt_zh_CN.qm next to the exe; in a dev build the Qt install's qt_zh_CN.qm pulls in qtbase.
    QTranslator qtTranslator;
    const QLocale chinese(QLocale::Chinese, QLocale::China);
    if (qtTranslator.load(chinese, QStringLiteral("qt"), QStringLiteral("_"), appDir.filePath("translations"))
        || qtTranslator.load(chinese, QStringLiteral("qt"), QStringLiteral("_"),
                             QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    QCommandLineParser parser;
    parser.addOption({"capture", "Start a screenshot (in the running instance, if there is one)."});
    parser.addOption({"autostart", "Launched by Windows at sign-in (set by the autostart option)."});
    parser.addOption({"self-check", "Check this installation without any UI and write a report.", "report-file"});
    parser.process(app);

    if (parser.isSet("self-check"))
        return runSelfCheck(parser.value("self-check"));

    // One instance per user (global hotkeys can only be registered once). A second launch
    // passes its request on, e.g. `jieying --capture` from a shortcut starts a capture.
    QLockFile lock(QDir::temp().filePath("jieying.lock"));
    if (!lock.tryLock(100)) {
        forwardToRunningInstance(parser.isSet("capture") ? "capture" : "activate");
        return 0;
    }
    // Lets the installer/uninstaller notice a running copy and ask to close it.
    HANDLE runningMutex = CreateMutexW(nullptr, FALSE, L"JieyingRunning");

    App jieying;
    if (!jieying.init())
        return 1;
    Autostart::refresh(); // keep the sign-in entry pointing here if the app was moved

    QLocalServer server;
    QLocalServer::removeServer(instanceChannel());
    server.listen(instanceChannel());
    QObject::connect(&server, &QLocalServer::newConnection, &jieying, [&server, &jieying] {
        while (QLocalSocket *client = server.nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            QObject::connect(client, &QLocalSocket::readyRead, &jieying, [client, &jieying] {
                while (client->canReadLine())
                    jieying.handleInstanceMessage(QString::fromUtf8(client->readLine()).trimmed());
            });
        }
    });

    if (parser.isSet("capture"))
        QTimer::singleShot(0, &jieying, [&jieying] { jieying.startCapture(); });
    const int result = app.exec();
    CloseHandle(runningMutex);
    return result;
}
