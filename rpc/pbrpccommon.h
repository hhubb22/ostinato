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

#ifndef _PB_RPC_COMMON_H
#define _PB_RPC_COMMON_H

#include <QByteArray>
#include <QtEndian>
#include <QtGlobal>

#include <climits>
#include <cstring>

// Print a HexDump
#define BUFDUMP(ptr, len) qDebug("%s", \
        qPrintable(QString(QByteArray((char*)(ptr), (len)).toHex()))); 

/*
** RPC Header (8)
**    - MSG_TYPE (2)
**    - METHOD_ID/NOTIF_TYPE (2)
**    - LEN (4) [not including this header]
*/
#define PB_HDR_SIZE                8

#define PB_MSG_TYPE_REQUEST        1
#define PB_MSG_TYPE_RESPONSE       2
#define PB_MSG_TYPE_BINBLOB        3
#define PB_MSG_TYPE_ERROR          4
#define PB_MSG_TYPE_NOTIFY         5

struct PbRpcHeader
{
    quint16 type;
    quint16 method;
    quint32 length;
};

enum PbRpcFrameStatus
{
    PbRpcFrameIncomplete,
    PbRpcFrameComplete,
    PbRpcFrameInvalid
};

inline bool pbRpcMessageTypeIsValid(quint16 type)
{
    return type >= PB_MSG_TYPE_REQUEST && type <= PB_MSG_TYPE_NOTIFY;
}

inline bool pbRpcPayloadLengthIsValid(quint32 length)
{
    return length <= quint32(INT_MAX - PB_HDR_SIZE);
}

inline void pbRpcEncodeHeader(char *header, quint16 type, quint16 method,
                              quint32 length)
{
    const quint16 beType = qToBigEndian(type);
    const quint16 beMethod = qToBigEndian(method);
    const quint32 beLength = qToBigEndian(length);

    std::memcpy(header, &beType, sizeof(beType));
    std::memcpy(header + 2, &beMethod, sizeof(beMethod));
    std::memcpy(header + 4, &beLength, sizeof(beLength));
}

inline bool pbRpcDecodeHeader(const uchar *data, int size, PbRpcHeader &header)
{
    if (size < PB_HDR_SIZE)
        return false;

    header.type = qFromBigEndian<quint16>(data);
    header.method = qFromBigEndian<quint16>(data + 2);
    header.length = qFromBigEndian<quint32>(data + 4);
    return true;
}

inline PbRpcFrameStatus pbRpcDecodeFrame(const QByteArray &data, int offset,
                                         PbRpcHeader &header,
                                         QByteArray &payload, int &frameSize)
{
    frameSize = 0;
    payload.clear();
    if (offset < 0 || offset > data.size())
        return PbRpcFrameInvalid;
    if (!pbRpcDecodeHeader(
            reinterpret_cast<const uchar *>(data.constData() + offset),
            data.size() - offset, header))
        return PbRpcFrameIncomplete;
    if (!pbRpcMessageTypeIsValid(header.type)
            || !pbRpcPayloadLengthIsValid(header.length))
        return PbRpcFrameInvalid;

    frameSize = PB_HDR_SIZE + int(header.length);
    if (data.size() - offset < frameSize)
        return PbRpcFrameIncomplete;

    payload = data.mid(offset + PB_HDR_SIZE, int(header.length));
    return PbRpcFrameComplete;
}

#endif
