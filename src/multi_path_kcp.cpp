#include <QtCore/qobject.h>
#include <QtCore/qscopeguard.h>
#include "kcp_base_p.h"
#include "../include/multi_path_kcp.h"
#include "../include/private/socket_p.h"

QTNETWORKNG_NAMESPACE_BEGIN

const char PACKET_TYPE_UNCOMPRESSED_DATA_WITH_TOKEN = 0x05;

// #define DEBUG_PROTOCOL 1
#define TOKEN_SIZE 256

int multi_path_kcp_client_callback(const char *buf, int len, ikcpcb *kcp, void *user);

class MultiPathUdpLinkClient
{
public:
    typedef QByteArray PathID;

    MultiPathUdpLinkClient();
    ~MultiPathUdpLinkClient();
public:
    bool connect(const QList<QPair<HostAddress, quint16>> &remoteHosts, int allowProtocol);
    // template
    qint32 recvfrom(char *data, qint32 size, QByteArray &who);
    qint32 sendto(const char *data, qint32 size, const QByteArray &who);
    bool filter(char *data, qint32 *size, QByteArray *who);
    void close();
    void abort();
    void closeSlave(const QByteArray &who){};
    void abortSlave(const QByteArray &who){};
    bool addSlave(const QByteArray &who, quint32 connectionId) { return false; };
public:
    void doReceive(QSharedPointer<Socket> rawSocket);
public:
    struct RemoteHost
    {
        HostAddress addr;
        quint16 port;
        QSharedPointer<Socket> rawSocket;

        bool operator==(const RemoteHost &other) { return addr == other.addr && port == other.port; }

        QString toString() const { return QString("%1:%2").arg(addr.toString(), QString::number(port)); }
    };
    QList<RemoteHost> remoteHosts;
    QByteArray token;  // size == TOKEN_SIZE

    QByteArray unhandleData;
    Event unhandleDataNotEmpty;
    Event unhandleDataEmpty;

    int receiver;

    // todo:
    // at first n*remoteHostPorts.size() packet priority sent to unsent address.
    // after that, priority sent to remote that can receive data
    int lastSend;
    int nextSend();  // make sure choose one remote to sent
};
typedef KcpBase<MultiPathUdpLinkClient> MultiPathKcpClient;

class MultiPathUdpLinkSlaveInfo
{
public:
    explicit MultiPathUdpLinkSlaveInfo(quint32 connectionId);
public:
    struct RemoteHost
    {
        HostAddress addr;
        quint16 port;
        QSharedPointer<Socket> rawSocket;

        quint64 lastActiveTimestamp;
        QString toString() const { return QString("%1:%2").arg(addr.toString(), QString::number(port)); }
    };
    QList<QSharedPointer<RemoteHost>> remoteHosts;

    int lastSend;
    quint32 connectionId;
    quint64 connectedTime;
    int nextSend();  // average sent to slave. if is not active, choose next
    qint32 send(const char *data, qint32 len);

    QSharedPointer<RemoteHost> append(const HostAddress &addr, quint16 port, QSharedPointer<Socket> rawSocket);
};
typedef MultiPathUdpLinkSlaveInfo::RemoteHost MultiPathUdpLinkSlaveOnePath;

class MultiPathUdpLinkServer
{
public:
    typedef QByteArray PathID;
    MultiPathUdpLinkServer();
    ~MultiPathUdpLinkServer();
public:
    bool bind(const QList<QPair<HostAddress, quint16>> &localHosts, Socket::BindMode mode = Socket::DefaultForPlatform);
public:
    // template
    qint32 recvfrom(char *data, qint32 size, QByteArray &who);
    qint32 sendto(const char *data, qint32 size, const QByteArray &who);
    bool filter(char *data, qint32 *size, QByteArray *who);
    void close();
    void abort();
    void closeSlave(const QByteArray &who);
    void abortSlave(const QByteArray &who);
    bool addSlave(const QByteArray &who, quint32 connectionId);
public:
    void doReceive(int localIndex);
    quint32 nextConnectionId();
public:
    struct Path
    {
        QMap<QByteArray, QSharedPointer<MultiPathUdpLinkSlaveOnePath>> tokenToOnePath;
        QSharedPointer<Socket> rawSocket;
    };

    QList<QSharedPointer<Path>> rawPaths;

    QMap<QByteArray, QSharedPointer<MultiPathUdpLinkSlaveInfo>> tokenToSlave;
    QMap<quint32, QByteArray> connectionIdToToken;

    QByteArray unhandleDataFromWho;
    QByteArray unhandleData;
    Event unhandleDataNotEmpty;
    Event unhandleDataEmpty;

    int receiver;
};

QByteArray makeMultiPathDataPacket(const QByteArray &token, const char *data, qint32 size)
{
    QByteArray packet(size + 1 + token.size(), Qt::Uninitialized);
    packet.data()[0] = PACKET_TYPE_UNCOMPRESSED_DATA_WITH_TOKEN;
    memcpy(packet.data() + 1, token.data(), token.size());
    memcpy(packet.data() + 1 + token.size(), data, static_cast<size_t>(size));
    return packet;
}

int multi_path_kcp_client_callback(const char *buf, int len, ikcpcb *kcp, void *user)
{
    MasterKcpBase<MultiPathUdpLinkClient> *p = static_cast<MasterKcpBase<MultiPathUdpLinkClient> *>(user);
    if (!p || !buf) {
        qtng_warning << "kcp_callback got invalid data.";
        return -1;
    }
    QByteArray packet;
    if (p->connectionId == 0) {
        if (len + TOKEN_SIZE > 65535) {
            qtng_warning << "kcp_callback got invalid multi data. len:" << len;
            return -1;
        }
        packet = makeMultiPathDataPacket(p->link->token, buf, len);
    } else {
        if (len > 65535) {
            qtng_warning << "kcp_callback got invalid data. len:" << len;
            return -1;
        }
        packet = MultiPathKcpClient::makeDataPacket(p->connectionId, buf, len);
    }
    qint32 sentBytes = p->sendRaw(packet.data(), packet.size());
    if (sentBytes != packet.size()) {  // but why this happens?
        if (p->error == Socket::NoError) {
            p->error = Socket::SocketAccessError;
            p->errorString = QString::fromLatin1("can not send udp packet");
        }
#ifdef DEBUG_PROTOCOL
        qtng_warning << "can not send packet to connection:" << p->connectionId;
#endif
        p->abort();
        return -1;
    }
    return sentBytes;
}

MultiPathUdpLinkClient::MultiPathUdpLinkClient()
    : token(randomBytes(TOKEN_SIZE))
    , lastSend(-1)
    , receiver(0)
{

    unhandleDataEmpty.set();
}

MultiPathUdpLinkClient::~MultiPathUdpLinkClient() { }

bool MultiPathUdpLinkClient::connect(const QList<QPair<HostAddress, quint16>> &remoteHosts, int allowProtocol)
{
    QSharedPointer<Socket> ipv4, ipv6;
    for (QPair<HostAddress, quint16> _ : remoteHosts) {
        RemoteHost remote;
        remote.addr = _.first;
        remote.port = _.second;
        if (remote.addr.isIPv4() == HostAddress::IPv4Protocol) {
            if (!(allowProtocol & HostAddress::IPv4Protocol)) {
                continue;
            }
            if (ipv4.isNull()) {
                ipv4.reset(new Socket(HostAddress::IPv4Protocol, Socket::UdpSocket));
                if (!ipv4->bind()) {
                    ipv4.clear();
                    continue;
                }
            }
            remote.rawSocket = ipv4;
        } else {
            if (!(allowProtocol & HostAddress::IPv6Protocol)) {
                continue;
            }
            if (ipv6.isNull()) {
                ipv6.reset(new Socket(HostAddress::IPv6Protocol, Socket::UdpSocket));
                if (!ipv6->bind()) {
                    ipv6.clear();
                    continue;
                }
            }
            remote.rawSocket = ipv6;
        }
        this->remoteHosts.append(remote);
    }
    return !this->remoteHosts.isEmpty();
}

qint32 MultiPathUdpLinkClient::recvfrom(char *data, qint32 size, QByteArray &)
{
    if (!unhandleDataNotEmpty.tryWait()) {
        return -1;
    }
    if (unhandleData.isEmpty()) {
        return 0;
    }
    qint32 result = unhandleData.size();
    Q_ASSERT(size >= result);
    memcpy(data, unhandleData.data(), result);
    unhandleData.clear();
    unhandleDataNotEmpty.clear();
    unhandleDataEmpty.set();
#ifdef DEBUG_PROTOCOL
    qtng_debug << "recv from udp packet" << result;
#endif
    return result;
}

qint32 MultiPathUdpLinkClient::sendto(const char *data, qint32 size, const QByteArray &)
{
    const RemoteHost &remote = remoteHosts.at(nextSend());
    QSharedPointer<Socket> rawSocket = remote.rawSocket;
    if (rawSocket.isNull()) {
        return -1;
    }
#ifdef DEBUG_PROTOCOL
    qtng_debug << "send udp packet" << size << "to:" << remote.toString() << (int) (data[0]);
#endif
    return rawSocket->sendto(data, size, remote.addr, remote.port);
}

bool MultiPathUdpLinkClient::filter(char *data, qint32 *size, QByteArray *who)
{
    return false;
}

void MultiPathUdpLinkClient::close()
{
    for (const RemoteHost &remote : remoteHosts) {
        if (remote.rawSocket) {
            remote.rawSocket->close();
        }
    }
    unhandleDataNotEmpty.clear();
    unhandleDataEmpty.clear();
    unhandleData.clear();
}

void MultiPathUdpLinkClient::abort()
{
    for (const RemoteHost &remote : remoteHosts) {
        if (remote.rawSocket) {
            remote.rawSocket->abort();
        }
    }
    unhandleDataNotEmpty.clear();
    unhandleDataEmpty.clear();
    unhandleData.clear();
}

void MultiPathUdpLinkClient::doReceive(QSharedPointer<Socket> rawSocket)
{
    auto cleanup = qScopeGuard([this] {
        if ((--receiver) > 0) {
            return;
        }
        unhandleData.clear();
        unhandleDataNotEmpty.set();
        unhandleDataEmpty.clear();
    });
    ++receiver;
    QByteArray buf(1024 * 64, Qt::Uninitialized);
    while (true) {
        qint32 len = rawSocket->recvfrom(buf.data(), buf.size(), nullptr, nullptr);
        if (len <= 0) {
#ifdef DEBUG_PROTOCOL
            qtng_debug << "multi path client can not receive udp packet. remote:"
                       << rawSocket->localAddressURI()
                       << "error:" << rawSocket->errorString();
#endif
            return;
        }
        do {
            if (!unhandleDataEmpty.tryWait()) {
#ifdef DEBUG_PROTOCOL
                qtng_debug << "wait unhandle data empty error:" << rawSocket->localAddressURI();
#endif
                return;
            }
        } while (!unhandleData.isEmpty());
        unhandleData = buf.left(len);
        unhandleDataNotEmpty.set();
        unhandleDataEmpty.clear();
    }
}

int MultiPathUdpLinkClient::nextSend()
{
    if ((++lastSend) < remoteHosts.size()) {
        return lastSend;
    }
    lastSend = 0;
    return lastSend;
}

int MultiPathUdpLinkSlaveInfo::nextSend()
{
    quint64 now = QDateTime::currentSecsSinceEpoch();
    int last = lastSend++;
    for (; lastSend < remoteHosts.size(); ++lastSend) {
        if (now <= remoteHosts.at(lastSend)->lastActiveTimestamp + 30) {
            return lastSend;
        }
    }
    for (lastSend = 0; lastSend < last; ++lastSend) {
        if (now <= remoteHosts.at(lastSend)->lastActiveTimestamp + 30) {
            return lastSend;
        }
    }
    return 0;
}

qint32 MultiPathUdpLinkSlaveInfo::send(const char *data, qint32 len)
{
    QSharedPointer<RemoteHost> remote = remoteHosts.at(nextSend());
    QSharedPointer<Socket> rawSocket = remote->rawSocket;
    if (rawSocket.isNull()) {
        return -1;
    }
    return rawSocket->sendto(data, len, remote->addr, remote->port);
}

QSharedPointer<MultiPathUdpLinkSlaveOnePath> MultiPathUdpLinkSlaveInfo::append(const HostAddress &addr, quint16 port,
                                                                               QSharedPointer<Socket> rawSocket)
{
    QSharedPointer<RemoteHost> remote(new RemoteHost());
    remote->addr = addr;
    remote->port = port;
    remote->rawSocket = rawSocket;
    remote->lastActiveTimestamp = QDateTime::currentSecsSinceEpoch();
    remoteHosts.append(remote);
    return remote;
}

MultiPathUdpLinkSlaveInfo::MultiPathUdpLinkSlaveInfo(quint32 connectionId)
    : lastSend(-1)
    , connectionId(connectionId)
    , connectedTime(QDateTime::currentSecsSinceEpoch())
{
}

MultiPathUdpLinkServer::MultiPathUdpLinkServer()
    : receiver(0)
{
    unhandleDataEmpty.set();
}

MultiPathUdpLinkServer::~MultiPathUdpLinkServer() { }

bool MultiPathUdpLinkServer::bind(const QList<QPair<HostAddress, quint16>> &localHosts,
                                  Socket::BindMode mode /*= Socket::DefaultForPlatform*/)
{
    for (const QPair<HostAddress, quint16> &_ : localHosts) {
        const HostAddress &addr = _.first;
        quint16 port = _.second;
        QSharedPointer<Socket> rawSocket;
        if (addr.isIPv4()) {
            rawSocket.reset(new Socket(HostAddress::IPv4Protocol, Socket::UdpSocket));
        } else {
            rawSocket.reset(new Socket(HostAddress::IPv6Protocol, Socket::UdpSocket));
        }
        if (mode & Socket::ReuseAddressHint) {
            rawSocket->setOption(Socket::AddressReusable, true);
        }
        if (!rawSocket->bind(addr, port)) {
            qtng_warning << "multi path bind addr:" << addr << "port:" << port << "error";
            continue;
        }
        QSharedPointer<Path> path(new Path());
        path->rawSocket = rawSocket;
        rawPaths.append(path);
    }
    return !rawPaths.isEmpty();
}

qint32 MultiPathUdpLinkServer::recvfrom(char *data, qint32 size, QByteArray &who)
{
    if (!unhandleDataNotEmpty.tryWait()) {
        return -1;
    }
    if (unhandleData.isEmpty()) {
        return 0;
    }
    who = unhandleDataFromWho;
    qint32 result = unhandleData.size();
    Q_ASSERT(size >= result);
    memcpy(data, unhandleData.data(), result);
    unhandleData.clear();
    unhandleDataFromWho.clear();
    unhandleDataNotEmpty.clear();
    unhandleDataEmpty.set();
#ifdef DEBUG_PROTOCOL
    qtng_debug << "recv from udp packet" << result;
#endif
    return result;
}

qint32 MultiPathUdpLinkServer::sendto(const char *data, qint32 size, const QByteArray &who)
{
    QSharedPointer<MultiPathUdpLinkSlaveInfo> slave = tokenToSlave.value(who);
    if (slave.isNull()) {
        return -1;
    }
#ifdef DEBUG_PROTOCOL
    qtng_debug << "send udp packet" << size;
#endif
    return slave->send(data, size);
}

bool MultiPathUdpLinkServer::filter(char *data, qint32 *size, QByteArray *who)
{
    return false;
}

void MultiPathUdpLinkServer::close()
{
    for (QSharedPointer<Path> rawPath : rawPaths) {
        rawPath->rawSocket->close();
    }
    unhandleDataFromWho.clear();
    unhandleDataNotEmpty.clear();
    unhandleDataEmpty.clear();
    unhandleData.clear();
}

void MultiPathUdpLinkServer::abort()
{
    for (QSharedPointer<Path> rawPath : rawPaths) {
        rawPath->rawSocket->abort();
    }
    unhandleDataFromWho.clear();
    unhandleDataNotEmpty.clear();
    unhandleDataEmpty.clear();
    unhandleData.clear();
}

void MultiPathUdpLinkServer::doReceive(int localIndex)
{
    auto cleanup = qScopeGuard([this] {
        if ((--receiver) > 0) {
            return;
        }
        unhandleData.clear();
        unhandleDataNotEmpty.set();
        unhandleDataEmpty.clear();
    });
    ++receiver;

    Q_ASSERT(localIndex >= 0 && localIndex < rawPaths.size());
    QSharedPointer<Path> rawPath = rawPaths.at(localIndex);
    QSharedPointer<Socket> rawSocket = rawPath->rawSocket;
    QMap<QByteArray, QSharedPointer<MultiPathUdpLinkSlaveOnePath>> &tokenToOnePath = rawPath->tokenToOnePath;

    QByteArray buf(1024 * 64, Qt::Uninitialized);
    QByteArray token;
    char *data = buf.data();
    HostAddress addr;
    quint16 port;

    while (true) {
        qint32 len = rawSocket->recvfrom(data, buf.size(), &addr, &port);
        if (len <= 0) {
#ifdef DEBUG_PROTOCOL
            qtng_debug << "multi path server can not receive udp packet.";
#endif
            return;
        }
        if (len < 5) {
#ifdef DEBUG_PROTOCOL
            qtng_debug << "got invalid kcp packet smaller than 5 bytes." << QByteArray(buf.data(), len);
#endif
            continue;
        }
        const char packType = data[0];
        if (packType == PACKET_TYPE_UNCOMPRESSED_DATA_WITH_TOKEN) {
            token = buf.mid(1, TOKEN_SIZE);
            if (token.size() != TOKEN_SIZE) {
                continue;
            }
#ifdef DEBUG_PROTOCOL
            token = token.toHex();
#endif
            QSharedPointer<MultiPathUdpLinkSlaveOnePath> onePath = tokenToOnePath.value(token);
            QSharedPointer<MultiPathUdpLinkSlaveInfo> slave;
            if (!onePath.isNull()) {
                // in this case we must find slave
                slave = tokenToSlave.value(token);
                if (slave.isNull()) {
#ifdef DEBUG_PROTOCOL
                    qtng_debug << "can not find slave:" << token << "addr:" << addr << "port:" << port;
#endif
                    continue;
                }
                // if pass 15s since last connected time, we reject the connection
                quint64 now = QDateTime::currentSecsSinceEpoch();
                if (now > 15 + slave->connectedTime) {
#ifdef DEBUG_PROTOCOL
                    qtng_debug << "reject data since pass " << (now - slave->connectedTime)
                               << "secs. connectionId:" << slave->connectionId;
#endif
                    continue;
                }
                onePath->lastActiveTimestamp = QDateTime::currentSecsSinceEpoch();
            } else {
                slave = tokenToSlave.value(token);
                if (slave.isNull()) {
                    slave.reset(new MultiPathUdpLinkSlaveInfo(0));
                    tokenToSlave.insert(token, slave);
                } else {
                    // if pass 15s since last connected time, we reject the connection
                    quint64 now = QDateTime::currentSecsSinceEpoch();
                    if (now > 15 + slave->connectedTime) {
#ifdef DEBUG_PROTOCOL
                        qtng_debug << "reject data since pass " << (now - slave->connectedTime)
                                   << "secs. connectionId:" << slave->connectionId;
#endif
                        continue;
                    }
                }
                onePath = slave->append(addr, port, rawSocket);
                tokenToOnePath.insert(token, onePath);
            }
            Q_ASSERT(!onePath.isNull());
            // trans data to PACKET_TYPE_UNCOMPRESSED_DATA
            data[0] = PACKET_TYPE_UNCOMPRESSED_DATA;
            memmove(data + 1, data + 1 + TOKEN_SIZE, len - 1 - TOKEN_SIZE);
            len -= TOKEN_SIZE;
        } else {
#if QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
            quint32 connectionId = qFromBigEndian<quint32>(data + 1);
#else
            quint32 connectionId = qFromBigEndian<quint32>(reinterpret_cast<uchar *>(data + 1));
#endif
            token = connectionIdToToken.value(connectionId);
            if (token.isEmpty()) {
#ifdef DEBUG_PROTOCOL
                qtng_debug << "reject data because can not find connection(1): " << connectionId;
#endif
                continue;
            }
            QSharedPointer<MultiPathUdpLinkSlaveOnePath> onePath = tokenToOnePath.value(token);
            if (onePath.isNull()) {
                QSharedPointer<MultiPathUdpLinkSlaveInfo> slave = tokenToSlave.value(token);
                if (slave.isNull()) { 
#ifdef DEBUG_PROTOCOL
                    qtng_debug << "reject data because can not find connection(2): " << connectionId;
#endif
                    continue;
                }

                onePath = slave->append(addr, port, rawSocket);
                tokenToOnePath.insert(token, onePath);
            } else {
                onePath->lastActiveTimestamp = QDateTime::currentSecsSinceEpoch();
            }
        }

        do {
            if (!unhandleDataEmpty.tryWait()) {
#ifdef DEBUG_PROTOCOL
                qtng_debug << "wait unhandle data empty error:" << rawSocket->localAddressURI();
#endif
                return;
            }
        } while (!unhandleData.isEmpty());
        unhandleDataFromWho = token;
        unhandleData = buf.left(len);
        unhandleDataNotEmpty.set();
        unhandleDataEmpty.clear();
    }
}

quint32 MultiPathUdpLinkServer::nextConnectionId()
{
    quint32 id;
    do {
#if QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
        id = qFromBigEndian<quint32>(randomBytes(4).constData());
#else
        id = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(randomBytes(4).constData()));
#endif
    } while (connectionIdToToken.contains(id));
    return id;
}

void MultiPathUdpLinkServer::closeSlave(const QByteArray &who)
{
    QSharedPointer<MultiPathUdpLinkSlaveInfo> slave = tokenToSlave.take(who);
    if (!slave) {
        return;
    }
    connectionIdToToken.remove(slave->connectionId);

    for (QSharedPointer<Path> rawPath : rawPaths) {
        rawPath->tokenToOnePath.remove(who);
    }
}

void MultiPathUdpLinkServer::abortSlave(const QByteArray &who)
{
    QSharedPointer<MultiPathUdpLinkSlaveInfo> slave = tokenToSlave.take(who);
    if (!slave) {
        return;
    }
    connectionIdToToken.remove(slave->connectionId);

    for (QSharedPointer<Path> rawPath : rawPaths) {
        rawPath->tokenToOnePath.remove(who);
    }
}

bool MultiPathUdpLinkServer::addSlave(const QByteArray &who, quint32 connectionId)
{
    QSharedPointer<MultiPathUdpLinkSlaveInfo> slave = tokenToSlave.value(who);
    if (!slave) {
        return false;
    }
    slave->connectionId = connectionId;
    connectionIdToToken.insert(connectionId, who);
    return true;
}

class MultiPathKcpClientSocketLike : public KcpBaseSocketLike<MultiPathUdpLinkClient>
{
public:
    MultiPathKcpClientSocketLike();
public:
    bool connect(const QList<QPair<HostAddress, quint16>> &remoteHosts, int allowProtocol);
};

class MultiPathKcpServerSocketLike : public KcpBaseSocketLike<MultiPathUdpLinkServer>
{
public:
    MultiPathKcpServerSocketLike();
protected:
    // interval
    MultiPathKcpServerSocketLike(KcpBase<MultiPathUdpLinkServer> *slave);
public:
    virtual bool bind(const HostAddress &address, quint16 port = 0,
                      Socket::BindMode mode = Socket::DefaultForPlatform) override
    {
        return true;
    }
    virtual bool bind(quint16 port = 0, Socket::BindMode mode = Socket::DefaultForPlatform) override { return true; }
    bool bind(const QList<QPair<HostAddress, quint16>> &localHosts, Socket::BindMode mode = Socket::DefaultForPlatform);

    virtual QSharedPointer<SocketLike> accept() override;
};

MultiPathKcpClientSocketLike::MultiPathKcpClientSocketLike()
    : KcpBaseSocketLike<MultiPathUdpLinkClient>(new MasterKcpBase<MultiPathUdpLinkClient>(
            QSharedPointer<MultiPathUdpLinkClient>(new MultiPathUdpLinkClient())))
{
}

bool MultiPathKcpClientSocketLike::connect(const QList<QPair<HostAddress, quint16>> &remoteHosts, int allowProtocol)
{
    if (!kcpBase->canConnect()) {
        return false;
    }
    MasterKcpBase<MultiPathUdpLinkClient> *master = dynamic_cast<MasterKcpBase<MultiPathUdpLinkClient> *>(kcpBase);
    if (!master) {
        return false;
    }
    QSharedPointer<MultiPathUdpLinkClient> link(master->link);
    if (!link->connect(remoteHosts, allowProtocol)) {
        return false;
    }
    kcpBase->setState(Socket::ConnectedState);

    QList<QSharedPointer<Socket>> rawSockets;
    for (int i = 0; i < link->remoteHosts.size(); ++i) {
        QSharedPointer<Socket> rawSocket = link->remoteHosts.at(i).rawSocket;
        if (!rawSockets.contains(rawSocket)) {
            rawSockets.append(rawSocket);
        }
    }
    if (rawSockets.isEmpty()) {
        return false;
    }
    for (int i = 0; i < rawSockets.size(); i++) {
        QSharedPointer<Socket> rawSocket = rawSockets.at(i);
        master->operations->spawnWithName("do_receive_" + QString::number(i),
                                          [link, rawSocket] { link->doReceive(rawSocket); });
    }
    return true;
}

MultiPathKcpServerSocketLike::MultiPathKcpServerSocketLike()
    : KcpBaseSocketLike<MultiPathUdpLinkServer>(new MasterKcpBase<MultiPathUdpLinkServer>(
            QSharedPointer<MultiPathUdpLinkServer>(new MultiPathUdpLinkServer())))
{
}

MultiPathKcpServerSocketLike::MultiPathKcpServerSocketLike(KcpBase<MultiPathUdpLinkServer> *slave)
    : KcpBaseSocketLike<MultiPathUdpLinkServer>(slave)
{
}

bool MultiPathKcpServerSocketLike::bind(const QList<QPair<HostAddress, quint16>> &localHosts,
                                        Socket::BindMode mode /*= Socket::DefaultForPlatform*/)
{
    if (!kcpBase->canBind()) {
        return false;
    }
    MasterKcpBase<MultiPathUdpLinkServer> *master = dynamic_cast<MasterKcpBase<MultiPathUdpLinkServer> *>(kcpBase);
    if (!master) {
        return false;
    }
    QSharedPointer<MultiPathUdpLinkServer> link(master->link);
    if (!link->bind(localHosts)) {
        return false;
    }
    kcpBase->setState(Socket::BoundState);
    for (int i = 0; i < link->rawPaths.size(); ++i) {
        master->operations->spawnWithName("do_accept_" + QString::number(i), [link, i] { link->doReceive(i); });
    }
    return true;
}

QSharedPointer<SocketLike> MultiPathKcpServerSocketLike::accept()
{
    KcpBase<MultiPathUdpLinkServer> *slave = kcpBase->accept();
    if (!slave) {
        return QSharedPointer<SocketLike>();
    }
    return QSharedPointer<MultiPathKcpServerSocketLike>(new MultiPathKcpServerSocketLike(slave));
}

QSharedPointer<SocketLike> createMultiPathKcpConnection(const QList<QPair<HostAddress, quint16>> &remoteHosts,
                                                        Socket::SocketError *error, int allowProtocol, KcpMode mode)
{
    QSharedPointer<MultiPathKcpClientSocketLike> socket(new MultiPathKcpClientSocketLike());
    if (!socket->connect(remoteHosts, allowProtocol)) {
        if (error) {
            *error = Socket::UnknownSocketError;
        }
        return nullptr;
    }
    socket->kcpBase->setMode(mode);
    socket->kcpBase->kcp->output = multi_path_kcp_client_callback;  // reset callback
    if (error) {
        *error = Socket::NoError;
    }
    return socket;
}

QSharedPointer<SocketLike>
createMultiPathKcpConnection(const QString &hostName, quint16 port, Socket::SocketError *error /*= nullptr*/,
                             QSharedPointer<SocketDnsCache> dnsCache /*= QSharedPointer<SocketDnsCache>()*/,
                             int allowProtocol /*= HostAddress::IPv4Protocol | HostAddress::IPv6Protocol*/,
                             KcpMode mode /*= Internet*/)
{
    QList<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.append(t);
    } else {
        if (dnsCache.isNull()) {
            addresses = Socket::resolve(hostName);
        } else {
            addresses = dnsCache->resolve(hostName);
        }
    }

    if (addresses.isEmpty()) {
        if (error) {
            *error = Socket::HostNotFoundError;
        }
        return nullptr;
    }
    QList<QPair<HostAddress, quint16>> remoteHosts;
    for (const HostAddress &host : addresses) {
        remoteHosts.append(qMakePair(host, port));
    }
    return createMultiPathKcpConnection(remoteHosts, error, allowProtocol, mode);
}

QSharedPointer<SocketLike> createMultiKcpServer(const QList<QPair<HostAddress, quint16>> &localHosts,
                                                int backlog /*= 50*/, KcpMode mode /*= Internet*/)
{
    QSharedPointer<MultiPathKcpServerSocketLike> socket(new MultiPathKcpServerSocketLike());
    if (!socket->bind(localHosts)) {
        return nullptr;
    }
    if (backlog > 0 && !socket->listen(backlog)) {
        return nullptr;
    }
    socket->kcpBase->setMode(mode);
    return socket;
}

QTNETWORKNG_NAMESPACE_END
