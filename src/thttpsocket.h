#ifndef THTTPSOCKET_H
#define THTTPSOCKET_H

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <TGlobal>
#include <THttpRequest>
#include <TTemporaryFile>


//class T_CORE_EXPORT THttpSocket : public QTcpSocket {
class T_CORE_EXPORT THttpSocket : public QObject {
    Q_OBJECT
public:
    THttpSocket(QObject *parent = 0);
    virtual ~THttpSocket();

    QList<THttpRequest> read();
    bool waitForReadyReadRequest(int msecs = 5000);
    bool canReadRequest() const;
    qint64 write(const THttpHeader *header, QIODevice *body);
    int idleTime() const;
    int socketId() const { return _sid; }
    void abort();
    void deleteLater();

    int socketDescriptor() const { return _socketDescriptor;}
    //bool setSocketDescriptor(qintptr socketDescriptor, SocketState socketState = ConnectedState, OpenMode openMode = ReadWrite);
    void setSocketDescriptor(int socketDescriptor, QAbstractSocket::SocketState socketState = QAbstractSocket::ConnectedState);
    QHostAddress peerAddress() const { return QHostAddress(); }
    QAbstractSocket::SocketState state() const { return _state; }

    static THttpSocket *searchSocket(int id);
    void writeRawDataFromWebSocket(const QByteArray &data);

protected:
    QByteArray readRawData(int msecs);

protected slots:
    qint64 writeRawData(const char *data, qint64 size);
    qint64 writeRawData(const QByteArray &data);

signals:
    void requestWrite(const QByteArray &data);  // internal use

private:
    T_DISABLE_COPY(THttpSocket)
    T_DISABLE_MOVE(THttpSocket)

    int _sid {0};
    int _socketDescriptor {0};
    QAbstractSocket::SocketState _state {QAbstractSocket::UnconnectedState};
    qint64 _lengthToRead {-1};
    QByteArray _readBuffer;
    TTemporaryFile _fileBuffer;
    quint64 _idleElapsed {0};

    friend class TActionThread;
};


inline bool THttpSocket::canReadRequest() const
{
    return (_lengthToRead == 0);
}

#endif  // THTTPSOCKET_H
