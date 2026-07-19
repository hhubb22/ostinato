/*
Copyright (C) 2010 Srivats P.

This file is part of "Ostinato"

This is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include "drone.h"

#include "../common/protocolmanager.h"
#include "params.h"
#include "settings.h"
#include "turbo.h"

#include <google/protobuf/stubs/common.h>

#ifndef OSTINATO_QT_FREE
#include <QCoreApplication>
#include <QFile>
#else
#include <chrono>
#include <filesystem>
#include <thread>
#include <unistd.h>
#endif

#include <signal.h>

extern ProtocolManager *OstProtocolManager;
extern const char *version;
extern const char *revision;

Drone *drone;
QSettings *appSettings;
Params appParams;

#ifndef OSTINATO_QT_FREE
void NoMsgHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);
#else
static volatile sig_atomic_t stopping = 0;
#endif

void cleanup(int /*signum*/)
{
#ifdef OSTINATO_QT_FREE
    stopping = 1;
#else
    fprintf(stderr, "\nCleaning up (may take a few seconds) ... ");
    fflush(stderr);
    QCoreApplication::instance()->exit(-1);
#endif
}

int main(int argc, char *argv[])
{
    int exitCode = 0;
#ifndef OSTINATO_QT_FREE
    QCoreApplication app(argc, argv);
    app.setApplicationName("Drone");
    app.setOrganizationName("Ostinato");
#endif

    appParams.parseCommandLine(argc, argv);

    fprintf(stderr, "Starting (will take a few seconds) ...\n");
    fflush(stderr);

#if defined(QT_NO_DEBUG) && !defined(OSTINATO_QT_FREE)
    if (appParams.optLogsDisabled())
        qInstallMessageHandler(NoMsgHandler);
#endif

    qDebug("Version: %s", version);
    qDebug("Revision: %s", revision);

    /* (Portable Mode) If we have a .ini file in the same directory as the 
       executable, we use that instead of the platform specific location
       and format for the settings */
#ifdef OSTINATO_QT_FREE
    char executablePath[4096] = {};
    const ssize_t pathLength = readlink("/proc/self/exe", executablePath,
                                        sizeof(executablePath)-1);
    QString portableIni((std::filesystem::path(pathLength > 0 ? executablePath : argv[0])
                         .parent_path()/"drone.ini").string());
    if (std::filesystem::exists(portableIni.toStdString()))
        appSettings = new QSettings(portableIni, QSettings::IniFormat);
    else
        appSettings = new QSettings("Ostinato", "drone");
#else
    QString portableIni = QCoreApplication::applicationDirPath() + "/drone.ini";
    if (QFile::exists(portableIni))
        appSettings = new QSettings(portableIni, QSettings::IniFormat);
    else
        appSettings = new QSettings(QSettings::IniFormat, 
                                    QSettings::UserScope,
                                    app.organizationName(), 
                                    app.applicationName().toLower());
    qDebug("Settings: %s", qPrintable(appSettings->fileName()));
#endif

    if (!initTurbo())
    {
        exitCode = 1;
        goto _exit2;
    }

    drone = new Drone();
    OstProtocolManager = new ProtocolManager();

    if (!drone->init())
    {
        exitCode = 1;
        goto _exit;
    }

    qDebug("Version: %s", version);
    qDebug("Revision: %s", revision);

#if defined(Q_OS_UNIX) || defined(OSTINATO_QT_FREE)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cleanup;
    if (sigaction(SIGTERM, &sa, NULL))
        qDebug("Failed to install SIGTERM handler. Cleanup may not happen!!!");
    if (sigaction(SIGINT, &sa, NULL))
        qDebug("Failed to install SIGINT handler. Cleanup may not happen!!!");
#elif defined(Q_OS_WIN32)
    if (signal(SIGTERM, cleanup) == SIG_ERR)
        qDebug("Failed to install SIGTERM handler. Cleanup may not happen!!!");
    if (signal(SIGINT, cleanup) == SIG_ERR)
        qDebug("Failed to install SIGINT handler. Cleanup may not happen!!!");
#endif

#ifdef OSTINATO_QT_FREE
    while (!stopping)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fprintf(stderr, "\nCleaning up (may take a few seconds) ... ");
    fflush(stderr);
#else
    exitCode = app.exec();
#endif

_exit:
    delete drone;
    delete OstProtocolManager;

_exit2:
    google::protobuf::ShutdownProtobufLibrary();

    fprintf(stderr, "done.\n");
    fflush(stderr);

    return exitCode;
} 

#ifndef OSTINATO_QT_FREE
void NoMsgHandler(QtMsgType type, const QMessageLogContext &/*context*/,
                const QString &msg)
{
    if (type == QtFatalMsg) {
        fprintf(stderr, "%s\n", qPrintable(msg));
        fflush(stderr);
        abort();
    }
}
#endif
