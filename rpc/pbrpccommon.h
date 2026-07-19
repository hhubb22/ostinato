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
#include <QtGlobal>

#include "pbrpc_core.h"

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
    return pbrpc::isValidMessageType(type);
}

inline bool pbRpcPayloadLengthIsValid(quint32 length)
{
    return length <= quint32(INT_MAX - PB_HDR_SIZE);
}

inline void pbRpcEncodeHeader(char *header, quint16 type, quint16 method,
                              quint32 length)
{
    const std::vector<std::uint8_t> encoded = pbrpc::encodeHeader(
        static_cast<pbrpc::MessageType>(type), method, length);
    std::memcpy(header, encoded.data(), encoded.size());
}

inline bool pbRpcDecodeHeader(const uchar *data, int size, PbRpcHeader &header)
{
    pbrpc::Header coreHeader;
    if (size < 0 || !pbrpc::decodeHeader(pbrpc::ByteView(data, std::size_t(size)),
                                         coreHeader))
        return false;
    header.type = static_cast<quint16>(coreHeader.type);
    header.method = coreHeader.method;
    header.length = coreHeader.length;
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
