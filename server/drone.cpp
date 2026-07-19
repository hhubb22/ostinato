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

#include "myservice.h"
#include "params.h"
#ifndef OSTINATO_QT_FREE
#include "rpcserver.h"
#include "../common/updater.h"
#include <QMetaType>
#endif
#include "settings.h"

extern Params appParams;
extern const char* version;
extern const char* revision;

Drone::Drone(QObject *parent)
     : QObject(parent)
{
#ifdef OSTINATO_QT_FREE
    service = new MyService();
    rpcServer = nullptr;
#else
    Updater *updater = new Updater();

#ifdef QT_DEBUG
    bool enableLogs = true;
#else
    bool enableLogs = !appParams.optLogsDisabled();
#endif

    rpcServer = new RpcServer(enableLogs);
    service = new MyService();

    connect(updater, SIGNAL(newVersionAvailable(QString)),
            this, SLOT(onNewVersion(QString)));
    updater->checkForNewVersion();
#endif
}

Drone::~Drone()
{
    delete rpcServer;
    delete service;
}

bool Drone::init()
{
    QString addr = appSettings->value(kRpcServerAddress).toString();
#ifdef OSTINATO_QT_FREE
    pbrpc::TcpRpcServer::Options options;
    options.address = addr.isEmpty() ? "0.0.0.0" : addr.toStdString();
    options.port = static_cast<std::uint16_t>(appParams.servicePortNumber());
    rpcServer = new pbrpc::TcpRpcServer(service, options);
    std::string error;
    if (!rpcServer->start(&error)) {
        qWarning("Unable to start RPC server: %s", error.c_str());
        return false;
    }
    return true;
#else
    QHostAddress address = addr.isEmpty() ?
        QHostAddress::Any : QHostAddress(addr);

    Q_ASSERT(rpcServer);

    qRegisterMetaType<SharedProtobufMessage>("SharedProtobufMessage");

    if (address.isNull()) {
        qWarning("Invalid RpcServer Address <%s> specified. Using 'Any'",
                qPrintable(addr));
        address = QHostAddress::Any;
    }

    if (!rpcServer->registerService(service, address, appParams.servicePortNumber()))
    {
        //qCritical(qPrintable(rpcServer->errorString()));
        return false;
    }

    connect(service, SIGNAL(notification(int, SharedProtobufMessage)), 
            rpcServer, SIGNAL(notifyClients(int, SharedProtobufMessage)));

    return true;
#endif
}

MyService* Drone::rpcService()
{
    return service;
}

void Drone::onNewVersion(QString newVersion)
{
    qWarning("%s", qPrintable(QString("New Ostinato version %1 available. "
                "Visit http://ostinato.org to download").arg(newVersion)));
}

#ifdef OSTINATO_QT_FREE
void Drone::notify(int type, const SharedProtobufMessage &message)
{
    if (rpcServer && message.data())
        rpcServer->broadcastNotification(static_cast<std::uint16_t>(type),
                                         *message.data());
}
#endif

