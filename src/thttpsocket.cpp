/* Copyright (c) 2010-2019, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include "thttpsocket.h"
#include "tatomicptr.h"
#include "tsystemglobal.h"
#include <QBuffer>
#include <QDir>
#include <TAppSettings>
#include <THttpHeader>
#include <THttpResponse>
#include <TMultipartFormData>
#include <TTemporaryFile>
#include <chrono>
#include <ctime>
#include <thread>
#ifdef Q_OS_UNIX
#include "tfcore_unix.h"
#endif

constexpr uint READ_THRESHOLD_LENGTH = 2 * 1024 * 1024;  // bytes
constexpr qint64 WRITE_LENGTH = 1408;
constexpr int WRITE_BUFFER_LENGTH = WRITE_LENGTH * 512;
//constexpr int SEND_BUF_SIZE = 64 * 1024;
constexpr int RECV_BUF_SIZE = 64 * 1024;
constexpr int RESERVED_BUFFER_SIZE = 1024;

namespace {
TAtomicPtr<THttpSocket> socketManager[USHRT_MAX + 1];
std::atomic<ushort> point {0};
}

/*!
  \class THttpSocket
  \brief The THttpSocket class provides a socket for the HTTP.
*/

THttpSocket::THttpSocket(QObject *parent) :
    QObject(parent)
{
    do {
        _sid = point.fetch_add(1);
    } while (!socketManager[_sid].compareExchange(nullptr, this));  // store a socket
    tSystemDebug("THttpSocket  sid:%d", _sid);

    connect(this, SIGNAL(requestWrite(const QByteArray &)), this, SLOT(writeRawData(const QByteArray &)), Qt::QueuedConnection);

    _idleElapsed = Tf::getMSecsSinceEpoch();
    _readBuffer.reserve(RESERVED_BUFFER_SIZE);
}


THttpSocket::~THttpSocket()
{
    abort();
    socketManager[_sid].compareExchangeStrong(this, nullptr);  // clear
    tSystemDebug("THttpSocket deleted  sid:%d", _sid);
}


QList<THttpRequest> THttpSocket::read()
{
    QList<THttpRequest> reqList;

    if (canReadRequest()) {
        if (_fileBuffer.isOpen()) {
            _fileBuffer.close();
            THttpRequest req(_readBuffer, _fileBuffer.fileName(), peerAddress());
            reqList << req;
            _readBuffer.resize(0);
            _fileBuffer.resize(0);
        } else {
            reqList = THttpRequest::generate(_readBuffer, peerAddress());
        }

        _lengthToRead = -1;
    }
    return reqList;
}


qint64 THttpSocket::write(const THttpHeader *header, QIODevice *body)
{
    if (body && !body->isOpen()) {
        if (!body->open(QIODevice::ReadOnly)) {
            tWarn("open failed");
            return -1;
        }
    }

    // Writes HTTP header
    QByteArray hdata = header->toByteArray();
    qint64 total = writeRawData(hdata.data(), hdata.size());
    if (total < 0) {
        return -1;
    }

    if (body) {
        QBuffer *buffer = qobject_cast<QBuffer *>(body);
        if (buffer) {
            if (writeRawData(buffer->data().data(), buffer->size()) != buffer->size()) {
                return -1;
            }
            total += buffer->size();
        } else {
            QByteArray buf(WRITE_BUFFER_LENGTH, 0);
            qint64 readLen = 0;
            while ((readLen = body->read(buf.data(), buf.size())) > 0) {
                if (writeRawData(buf.data(), readLen) != readLen) {
                    return -1;
                }
                total += readLen;
            }
        }
    }
    return total;
}


QByteArray THttpSocket::readRawData(int msecs)
{
    int total = 0;
    int timeout = 0;
    QByteArray buffer;
    buffer.reserve(RECV_BUF_SIZE);

    int res = tf_poll_recv(socketDescriptor(), msecs);
    if (res < 0) {
        tSystemError("socket poll error");
        abort();
        return buffer;
    }

    if (!res) {
        // timeout
        return buffer;
    }

    qint64 startidle = Tf::getMSecsSinceEpoch();

    do {
        int buflen = RECV_BUF_SIZE - total;
        int len = tf_recv(socketDescriptor(), buffer.data() + total, buflen);
        int error = errno;
        tSystemInfo("len: %d", len);

        if (len < 0) {
            if (error == EAGAIN) {
                if (Tf::getMSecsSinceEpoch() - startidle < msecs) {
                    std::this_thread::yield();
                    continue;
                } else {
                    break;
                }
            }
            abort();
            break;

        } else if (len == 0) {
            tSystemError("#### Remote disconected");
            abort();
            break;

        } else {
            _idleElapsed = Tf::getMSecsSinceEpoch();
            total += len;
            buffer.resize(total);

            if (len < buflen || total == RECV_BUF_SIZE) {
                break;
            }

            timeout = Tf::getMSecsSinceEpoch() - startidle - msecs;
            if (timeout <= 0) {
                break;
            }
        }

    } while (tf_poll_recv(socketDescriptor(), timeout) > 0);

    return buffer;
}


void THttpSocket::writeRawDataFromWebSocket(const QByteArray &data)
{
    emit requestWrite(data);
}


qint64 THttpSocket::writeRawData(const char *data, qint64 size)
{
    qint64 total = 0;

    if (_socketDescriptor <= 0) {
        return -1;
    }

    if (Q_UNLIKELY(!data || size == 0)) {
        return total;
    }

    for (;;) {
        int res = tf_poll_send(_socketDescriptor, 1000);
        //int res = 1;
        if (res <= 0) {
            abort();
            break;
        } else {
            qint64 written = tf_write(_socketDescriptor, data + total, qMin(size - total, WRITE_LENGTH));
            if (Q_UNLIKELY(written <= 0)) {
                tWarn("socket write error: total:%d (%d)", (int)total, (int)written);
                return -1;
            }

            total += written;
            if (total >= size) {
                break;
            }
        }
    }

    _idleElapsed = Tf::getMSecsSinceEpoch();
    return total;
}


qint64 THttpSocket::writeRawData(const QByteArray &data)
{
    return writeRawData(data.data(), data.size());
}


bool THttpSocket::waitForReadyReadRequest(int msecs)
{
    static const qint64 systemLimitBodyBytes = Tf::appSettings()->value(Tf::LimitRequestBody, "0").toLongLong() * 2;

    // int res = tf_poll_recv(socketDescriptor(), msecs);
    // if (res < 0) {
    //     tSystemWarn("socket poll error");
    //     abort();
    //     return false;
    // }

    // if (!res) {
    //     // timeout
    //     return canReadRequest();
    // }

    // QByteArray buf;
    // QByteArray buffer;
    // buffer.reserve(RECV_BUF_SIZE);
    // qint64 startidle = Tf::getMSecsSinceEpoch();
    // do {
    //     int len = tf_recv(socketDescriptor(), buffer.data(), RECV_BUF_SIZE, 0);
    //     int error = errno;
    //     if (len < 0) {
    //         if (error == EAGAIN) {
    //             if (Tf::getMSecsSinceEpoch() - startidle < 1000) {
    //                 std::this_thread::yield();
    //                 continue;
    //             } else {
    //                 return canReadRequest();
    //             }
    //         }
    //         abort();
    //         break;
    //     }

    //     if (len == 0) {
    //         abort();
    //         break;
    //     }

    //     buffer.resize(len);
    //     buf += buffer;
    //     if (len < RECV_BUF_SIZE) {
    //         break;
    //     }
    //     //timeout = 1;
    // } while (tf_poll_recv(socketDescriptor(), 1) > 0);

    auto buf = readRawData(msecs);
    if (!buf.isEmpty()) {
        if (_lengthToRead > 0) {
            // Writes to buffer
            if (_fileBuffer.isOpen()) {
                if (_fileBuffer.write(buf.data(), buf.size()) < 0) {
                    throw RuntimeException(QLatin1String("write error: ") + _fileBuffer.fileName(), __FILE__, __LINE__);
                }
            } else {
                _readBuffer.append(buf);
            }
            _lengthToRead = qMax(_lengthToRead - buf.size(), 0LL);

        } else if (_lengthToRead < 0) {
            _readBuffer.append(buf);
            int idx = _readBuffer.indexOf(Tf::CRLFCRLF);
            if (idx > 0) {
                THttpRequestHeader header(_readBuffer);
                tSystemDebug("content-length: %lld", header.contentLength());

                if (Q_UNLIKELY(systemLimitBodyBytes > 0 && header.contentLength() > systemLimitBodyBytes)) {
                    throw ClientErrorException(Tf::RequestEntityTooLarge);  // Request Entity Too Large
                }

                _lengthToRead = qMax(idx + 4 + (qint64)header.contentLength() - _readBuffer.length(), 0LL);

                if (header.contentLength() > READ_THRESHOLD_LENGTH || (header.contentLength() > 0 && header.contentType().trimmed().startsWith("multipart/form-data"))) {
                    // Writes to file buffer
                    if (Q_UNLIKELY(!_fileBuffer.open())) {
                        throw RuntimeException(QLatin1String("temporary file open error: ") + _fileBuffer.fileTemplate(), __FILE__, __LINE__);
                    }
                    if (_readBuffer.length() > idx + 4) {
                        tSystemDebug("fileBuffer name: %s", qPrintable(_fileBuffer.fileName()));
                        if (_fileBuffer.write(_readBuffer.data() + idx + 4, _readBuffer.length() - (idx + 4)) < 0) {
                            throw RuntimeException(QLatin1String("write error: ") + _fileBuffer.fileName(), __FILE__, __LINE__);
                        }
                    }
                }
            }
        } else {
            // do nothing
        }
    }
    return canReadRequest();
}


// qint64 THttpSocket::getContentLength()
// {
//     return 0;
// }

// bool THttpSocket::setSocketDescriptor(qintptr socketDescriptor, SocketState socketState, OpenMode openMode)
// {
//     bool ret = QTcpSocket::setSocketDescriptor(socketDescriptor, socketState, openMode);
//     if (ret) {
//         // Sets socket options
//         QTcpSocket::setSocketOption(QAbstractSocket::LowDelayOption, 1);

//         // Sets buffer size of socket
//         int val = QTcpSocket::socketOption(QAbstractSocket::SendBufferSizeSocketOption).toInt();
//         if (val < SEND_BUF_SIZE) {
//             QTcpSocket::setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, SEND_BUF_SIZE);
//         }

//         val = QTcpSocket::socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toInt();
//         if (val < RECV_BUF_SIZE) {
//             QTcpSocket::setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, RECV_BUF_SIZE);
//         }
// #ifdef Q_OS_UNIX
//         int bufsize = SEND_BUF_SIZE;
//         int res = setsockopt((int)socketDescriptor, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

//         if (res < 0) {
//             tSystemWarn("setsockopt error [SO_SNDBUF] fd:%d", (int)socketDescriptor);
//         }

//         bufsize = RECV_BUF_SIZE;
//         res = setsockopt((int)socketDescriptor, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
//         if (res < 0) {
//             tSystemWarn("setsockopt error [SO_RCVBUF] fd:%d", (int)socketDescriptor);
//         }
// #endif
//     } else {
//         tSystemWarn("Error setSocketDescriptor: %lld", socketDescriptor);
//     }
//     return ret;
// }


void THttpSocket::abort()
{
    if (_socketDescriptor > 0) {
        tf_close(_socketDescriptor);
        tSystemWarn("close: %d", _socketDescriptor);
        _state = QAbstractSocket::ClosingState;
        _socketDescriptor = 0;
    } else {
        _state = QAbstractSocket::UnconnectedState;
    }
}

void THttpSocket::deleteLater()
{
    socketManager[_sid].compareExchange(this, nullptr);  // clear
    QObject::deleteLater();
}


THttpSocket *THttpSocket::searchSocket(int sid)
{
    return socketManager[sid & 0xffff].load();
}

/*!
  Returns the number of seconds of idle time.
*/
int THttpSocket::idleTime() const
{
    return (Tf::getMSecsSinceEpoch() - _idleElapsed) / 1000;
}


void THttpSocket::setSocketDescriptor(int socketDescriptor, QAbstractSocket::SocketState socketState)
{
    _socketDescriptor = socketDescriptor;
    _state = socketState;
}

/*!
  Returns true if a HTTP request was received entirely; otherwise
  returns false.
  \fn bool THttpSocket::canReadRequest() const
*/
