﻿#include "sshclient.h"
#include <QTemporaryFile>
#include <QDir>
#include <QEventLoop>
#include <QDateTime>
#include <QCoreApplication>
#include "sshtunnelin.h"
#include "sshtunnelout.h"
#include "sshprocess.h"
#include "sshscpsend.h"
#include "sshscpget.h"
#include "sshsftp.h"
#include "cerrno"

Q_LOGGING_CATEGORY(sshclient, "ssh.client", QtWarningMsg)

#if !defined(MAX_LOST_KEEP_ALIVE)

/*
 * Maximum keep alive cycle (generally 5s) before connection is
 * considered as lost
 * Default: 6 (30s)
 */
#define MAX_LOST_KEEP_ALIVE 6
#endif

int SshClient::s_nbInstance = 0;

static ssize_t qt_callback_libssh_recv(int socket,void *buffer, size_t length,int flags, void **abstract)
{
    Q_UNUSED(socket)
    Q_UNUSED(flags)

    QTcpSocket * c = reinterpret_cast<QTcpSocket *>(* abstract);
    qint64 r = c->read(reinterpret_cast<char *>(buffer), static_cast<qint64>(length));
    if (r == 0)
    {
        return -EAGAIN;
    }
    return static_cast<ssize_t>(r);
}

static ssize_t qt_callback_libssh_send(int socket,const void * buffer, size_t length,int flags, void ** abstract)
{
    Q_UNUSED(socket)
    Q_UNUSED(flags)

    QTcpSocket * c = reinterpret_cast<QTcpSocket *>(* abstract);
    qint64 r = c->write(reinterpret_cast<const char *>(buffer), static_cast<qint64>(length));
    if (r == 0)
    {
        return -EAGAIN;
    }
    return static_cast<ssize_t>(r);
}

SshClient::SshClient(const QString &name, QObject * parent):
    QObject(parent),
    m_name(name),
    m_socket(this)
{
    /* New implementation */
    QObject::connect(this, &SshClient::sshEvent, this, &SshClient::_ssh_processEvent, Qt::QueuedConnection);
    QObject::connect(&m_socket, &QTcpSocket::connected,      this, &SshClient::_connection_socketConnected);
    QObject::connect(&m_socket, &QTcpSocket::disconnected,   this, &SshClient::_connection_socketDisconnected);
    QObject::connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
                                                             this, &SshClient::_connection_socketError);
    QObject::connect(&m_socket, &QTcpSocket::readyRead,      this, &SshClient::_ssh_processEvent, Qt::QueuedConnection);
    QObject::connect(&m_connectionTimeout, &QTimer::timeout, this, &SshClient::_connection_socketTimeout);
    QObject::connect(&m_keepalive,&QTimer::timeout,          this, &SshClient::_sendKeepAlive);

    if(s_nbInstance == 0)
    {
        qCDebug(sshclient) << m_name << ": libssh2_init()";
        Q_ASSERT(libssh2_init(0) == 0);
    }
    ++s_nbInstance;

    qCDebug(sshclient) << m_name << ": created " << this;
}

SshClient::~SshClient()
{
    qCDebug(sshclient) << m_name << ": SshClient::~SshClient() " << this;
    disconnectFromHost();
    waitForState(SshClient::SshState::Unconnected);
    --s_nbInstance;
    if(s_nbInstance == 0)
    {
        qCDebug(sshclient) << m_name << ": libssh2_exit()";
        libssh2_exit();
    }
    qCDebug(sshclient) << m_name << ": destroyed";
}

QString SshClient::getName() const
{
    return m_name;
}

bool SshClient::takeChannelCreationMutex(void *identifier)
{
    if ( ! channelCreationInProgress.tryLock() && currentLockerForChannelCreation != identifier )
    {
        qCDebug(sshclient) << "takeChannelCreationMutex another channel is already been created, have to wait";
        return false;
    }
    currentLockerForChannelCreation = identifier;
    return true;
}

void SshClient::releaseChannelCreationMutex(void *identifier)
{
    if ( currentLockerForChannelCreation == identifier )
    {
        channelCreationInProgress.unlock();
        currentLockerForChannelCreation = nullptr;
    }
    else
    {
        qCCritical(sshclient) << "Trying to release channel mutex but it doesn't host it";
    }
}

LIBSSH2_SESSION *SshClient::session()
{
    return m_session;
}

bool SshClient::loopWhileBytesWritten(int msecs)
{

    QEventLoop wait;
    QTimer timeout;
    bool written;
    auto con1 = QObject::connect(&m_socket, &QTcpSocket::bytesWritten, [this, &written, &wait]()
    {
        qCDebug(sshclient, "%s: BytesWritten", qPrintable(m_name));
        written = true;
        wait.quit();
    });
    auto con2 = QObject::connect(&timeout, &QTimer::timeout, [this, &written, &wait]()
    {
        qCWarning(sshclient, "%s: Bytes Write Timeout", qPrintable(m_name));
        written = false;
        wait.quit();
    });
    auto con3 = QObject::connect(&m_socket, static_cast<void (QTcpSocket::*)(QAbstractSocket::SocketError)>(&QTcpSocket::error), [this, &written, &wait]()
    {
        qCWarning(sshclient, "%s: Socket Error", qPrintable(m_name));
        written = false;
        wait.quit();
    });
    timeout.start(msecs); /* Timeout 10s */
    wait.exec();
    QObject::disconnect(con1);
    QObject::disconnect(con2);
    QObject::disconnect(con3);
    return written;
}


int SshClient::connectToHost(const QString & user, const QString & host, quint16 port, QByteArrayList methodes)
{
    if(sshState() != SshState::Unconnected)
    {
        qCCritical(sshclient) << m_name << "Allready connected";
        return 0;
    }

    m_authenticationMethodes = methodes;
    m_hostname = host;
    m_port = port;
    m_username = user;

    setSshState(SshState::SocketConnection);
    emit sshEvent();
    return 0;
}

bool SshClient::waitForState(SshState state)
{
    QEventLoop wait;
    QObject::connect(this, &SshClient::sshStateChanged, &wait, &QEventLoop::quit);
    while(sshState() != SshState::Error && sshState() != state)
    {
        wait.exec();
    }
    return sshState() == state;
}


void SshClient::disconnectFromHost()
{
    if(m_sshState == SshState::Unconnected)
        return;

    qCDebug(sshclient) << m_name << ": disconnectFromHost()";
    if(m_channels.size() == 0)
    {
        setSshState(DisconnectingSession);
    }
    else
    {
        setSshState(DisconnectingChannel);
    }

    emit sshEvent();
    return;
}


void SshClient::setPassphrase(const QString & pass)
{
    m_passphrase = pass;
}

void SshClient::setKeys(const QString &publicKey, const QString &privateKey)
{
    m_publicKey  = publicKey;
    m_privateKey = privateKey;
}

bool SshClient::saveKnownHosts(const QString & file)
{
    bool res = (libssh2_knownhost_writefile(m_knownHosts, qPrintable(file), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0);
    return res;
}

void SshClient::setKownHostFile(const QString &file)
{
    m_knowhostFiles = file;
}

bool SshClient::addKnownHost(const QString & hostname,const SshKey & key)
{
    bool ret;
    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    switch (key.type)
    {
        case SshKey::Dss:
            typemask |= LIBSSH2_KNOWNHOST_KEY_SSHDSS;
            break;
        case SshKey::Rsa:
            typemask |= LIBSSH2_KNOWNHOST_KEY_SSHRSA;
            break;
        case SshKey::UnknownType:
            return false;
    }
    ret = (libssh2_knownhost_add(m_knownHosts, qPrintable(hostname), nullptr, key.key.data(), static_cast<size_t>(key.key.size()), typemask, nullptr));
    return ret;
}

QString SshClient::banner()
{
    return QString(libssh2_session_banner_get(m_session));
}

void SshClient::_sendKeepAlive()
{
    int keepalive = 0;
    if(m_session)
    {
        int ret = libssh2_keepalive_send(m_session, &keepalive);
        if(ret == LIBSSH2_ERROR_SOCKET_SEND)
        {
            qCWarning(sshclient) << m_name << ": Connection I/O error !!!";
            m_socket.disconnectFromHost();
        }
        else if(((QDateTime::currentMSecsSinceEpoch() - m_lastProofOfLive) / 1000) > (MAX_LOST_KEEP_ALIVE * keepalive))
        {
            qCWarning(sshclient) << m_name << ": Connection lost !!!";
            m_socket.disconnectFromHost();
        }
        else
        {
            keepalive -= 1;
            if(keepalive < 2) keepalive = 2;
            m_keepalive.start(keepalive * 1000);
        }
    }
}

SshClient::SshState SshClient::sshState() const
{
    return m_sshState;
}

void SshClient::setSshState(const SshState &sshState)
{
    if(m_sshState != sshState)
    {
        m_sshState = sshState;
        emit sshStateChanged(m_sshState);
    }
}


void SshClient::unregisterChannel(SshChannel *channel)
{
    qCDebug(sshclient) << m_name << ": Ask to unregister " << channel->name();
    m_channels.removeOne(channel);

    if(sshState() == SshState::DisconnectingChannel && m_channels.size() == 0)
    {

        qCDebug(sshclient) << m_name << ": no more channel registered";

        /* Stop keepalive */
        m_keepalive.stop();

        setSshState(SshState::DisconnectingSession);
        emit sshEvent();
    }
}

void SshClient::registerChannel(SshChannel *channel)
{
    qCDebug(sshclient) << m_name << ": Ask to register " << channel->name();
    m_channels.append(channel);
}

void SshClient::setName(const QString &name)
{
    m_name = name;
}

/* New implementation */
const int ConnectionTimeout = 60000;



void SshClient::_connection_socketTimeout()
{
    m_socket.disconnectFromHost();
    qCWarning(sshclient) << m_name << ": ssh socket connection timeout";
    setSshState(SshState::Error);
    emit sshEvent();
}

void SshClient::_connection_socketError()
{
    qCWarning(sshclient) << m_name << ": ssh socket connection error:" << m_sshState;
    setSshState(SshState::Error);
    emit sshEvent();
}

void SshClient::_connection_socketConnected()
{
    qCDebug(sshclient) << m_name << ": ssh socket connected";

    if(m_sshState == SshState::WaitingSocketConnection)
    {
        /* Normal process; socket is connected */
        setSshState(SshState::Initialize);
        emit sshEvent();
    }
    else
    {
        qCWarning(sshclient) << m_name << ": Unknown conenction on socket";
        setSshState(SshState::Error);
        emit sshEvent();
    }
}

void SshClient::_connection_socketDisconnected()
{
    qCWarning(sshclient) << m_name << ": ssh socket disconnected";
    setSshState(FreeSession);
    emit sshEvent();
}

void SshClient::_ssh_processEvent()
{
    switch(m_sshState)
    {
        case SshState::Unconnected:
        {
            qCWarning(sshclient) << m_name << ": Unknown data on socket";
            return;
        }

        case SshState::SocketConnection:
        {
            m_connectionTimeout.start(ConnectionTimeout);
            m_socket.connectToHost(m_hostname, m_port);
            setSshState(SshState::WaitingSocketConnection);
        }

        FALLTHROUGH; case SshState::WaitingSocketConnection:
        {
            qCWarning(sshclient) << m_name << ": Unknown data on socket";
            return;
        }

        case SshState::Initialize:
        {
            m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, reinterpret_cast<void *>(&m_socket));
            if(m_session == nullptr)
            {
                qCCritical(sshclient) << m_name << ": libssh error during session init";
                setSshState(SshState::Error);
                m_socket.disconnectFromHost();
                return;
            }

            libssh2_session_callback_set(m_session, LIBSSH2_CALLBACK_RECV,reinterpret_cast<void*>(& qt_callback_libssh_recv));
            libssh2_session_callback_set(m_session, LIBSSH2_CALLBACK_SEND,reinterpret_cast<void*>(& qt_callback_libssh_send));
            libssh2_session_set_blocking(m_session, 0);

            m_knownHosts = libssh2_knownhost_init(m_session);
            Q_ASSERT(m_knownHosts);

            if(m_knowhostFiles.size())
            {
                libssh2_knownhost_readfile(m_knownHosts, qPrintable(m_knowhostFiles), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
            }

            setSshState(SshState::HandShake);
        }

        FALLTHROUGH; case SshState::HandShake:
        {
            int ret = libssh2_session_handshake(m_session, static_cast<int>(m_socket.socketDescriptor()));
            if(ret == LIBSSH2_ERROR_EAGAIN)
            {
                return;
            }
            if(ret != 0)
            {
                qCCritical(sshclient) << m_name << "Handshake error" << sshErrorToString(ret);
                setSshState(SshState::Error);
                m_socket.disconnectFromHost();
                return;
            }

            /* HandShake success, continue autentication */
            size_t len;
            int type;
            const char * fingerprint = libssh2_session_hostkey(m_session, &len, &type);
            if(fingerprint == nullptr)
            {
                qCCritical(sshclient) << m_name << "Fingerprint error";
                setSshState(SshState::Error);
                m_socket.disconnectFromHost();
                return;
            }

            m_hostKey.hash = QByteArray(libssh2_hostkey_hash(m_session,LIBSSH2_HOSTKEY_HASH_MD5), 16);
            switch (type)
            {
                case LIBSSH2_HOSTKEY_TYPE_RSA:
                    m_hostKey.type=SshKey::Rsa;
                    break;
                case LIBSSH2_HOSTKEY_TYPE_DSS:
                    m_hostKey.type=SshKey::Dss;
                    break;
                default:
                    m_hostKey.type=SshKey::UnknownType;
            }

            m_hostKey.key = QByteArray(fingerprint, static_cast<int>(len));
            struct libssh2_knownhost *khost;
            libssh2_knownhost_check(m_knownHosts, m_hostname.toStdString().c_str(), fingerprint, len, LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW, &khost);
            setSshState(SshState::GetAuthenticationMethodes);
        }

        FALLTHROUGH; case SshState::GetAuthenticationMethodes:
        {
            if(m_authenticationMethodes.length() == 0)
            {
                QByteArray username = m_username.toLocal8Bit();
                char * alist = nullptr;

                alist = libssh2_userauth_list(m_session, username.data(), static_cast<unsigned int>(username.length()));
                if(alist == nullptr)
                {
                    int ret = libssh2_session_last_error(m_session, nullptr, nullptr, 0);
                    if(ret == LIBSSH2_ERROR_EAGAIN)
                    {
                        return;
                    }
                    setSshState(SshState::Error);
                    m_socket.disconnectFromHost();
                    qCDebug(sshclient) << m_name << ": Failed to authenticate:" << sshErrorToString(ret);
                    return;
                }
                qCDebug(sshclient) << m_name << ": ssh start authentication userauth_list: " << alist;

                m_authenticationMethodes = QByteArray(alist).split(',');
            }
            setSshState(SshState::Authentication);
        }

        FALLTHROUGH; case SshState::Authentication:
        {
            while(m_authenticationMethodes.length() != 0)
            {
                if(m_authenticationMethodes.first() == "publickey")
                {
                    int ret = libssh2_userauth_publickey_frommemory(
                                    m_session,
                                    m_username.toStdString().c_str(),
                                    static_cast<size_t>(m_username.length()),
                                    m_publicKey.toStdString().c_str(),
                                    static_cast<size_t>(m_publicKey.length()),
                                    m_privateKey.toStdString().c_str(),
                                    static_cast<size_t>(m_privateKey.length()),
                                    m_passphrase.toStdString().c_str()
                            );
                    if(ret == LIBSSH2_ERROR_EAGAIN)
                    {
                        return;
                    }
                    if(ret < 0)
                    {
                        qCWarning(sshclient) << m_name << ": Authentication with publickey failed:" << sshErrorToString(ret);
                        m_authenticationMethodes.removeFirst();
                    }
                    if(ret == 0)
                    {
                        qCWarning(sshclient) << m_name << ": Authenticated with publickey";
                        setSshState(SshState::Ready);
                        break;
                    }
                }

                if(m_authenticationMethodes.first() == "password")
                {
                    QByteArray username = m_username.toLatin1();
                    QByteArray passphrase = m_passphrase.toLatin1();

                    int ret = libssh2_userauth_password_ex(m_session,
                                                             username.data(),
                                                             static_cast<unsigned int>(username.length()),
                                                             passphrase.data(),
                                                             static_cast<unsigned int>(passphrase.length()), nullptr);
                    if(ret == LIBSSH2_ERROR_EAGAIN)
                    {
                        return;
                    }
                    if(ret < 0)
                    {
                        qCWarning(sshclient) << m_name << ": Authentication with password failed";
                        m_authenticationMethodes.removeFirst();
                        return;
                    }
                    if(ret == 0)
                    {
                        qCDebug(sshclient) << m_name << ": Authenticated with password";
                        setSshState(SshState::Ready);
                        break;
                    }
                }
            }
            if(libssh2_userauth_authenticated(m_session))
            {
                qCDebug(sshclient) << m_name << ": Connected and authenticated";
                m_connectionTimeout.stop();
                m_keepalive.setSingleShot(true);
                m_keepalive.start(1000);
                libssh2_keepalive_config(m_session, 1, 5);
                setSshState(SshState::Ready);
                emit sshReady();
            }
            else
            {
                qCWarning(sshclient) << m_name << ": Authentication failed";
                setSshState(SshState::Error);
                return;
            }
            FALLTHROUGH;
        }

        case SshState::Ready:
        {
            m_lastProofOfLive = QDateTime::currentMSecsSinceEpoch();
            emit sshDataReceived();
            return;
        }

        case SshState::DisconnectingChannel:
        {
            /* Close all Opened Channels */
            if(m_channels.size() > 0)
            {
                for(SshChannel* ch: m_channels)
                {
                    ch->close();
                }
            }
            else
            {
                setSshState(DisconnectingSession);
            }
            break;
        }

        case SshState::DisconnectingSession:
        {
            int ret = libssh2_session_disconnect_ex(m_session, SSH_DISCONNECT_BY_APPLICATION, "good bye!", "");
            if(ret == LIBSSH2_ERROR_EAGAIN)
            {
                return;
            }
            if(m_socket.state() == QAbstractSocket::ConnectedState)
            {
                m_socket.disconnectFromHost();
            }
            else
            {
                setSshState(FreeSession);
                emit sshEvent();
            }
        }

        FALLTHROUGH; case SshState::FreeSession:
        {
            if (m_knownHosts)
            {
                libssh2_knownhost_free(m_knownHosts);
                m_knownHosts = nullptr;
            }

            if(m_session)
            {
                int ret = libssh2_session_free(m_session);
                if(ret == LIBSSH2_ERROR_EAGAIN)
                {
                    return;
                }
            }

            m_session = nullptr;
            emit sshDisconnected();
            setSshState(Unconnected);
            break;
        }

        case SshState::Error:
        {
            if(m_socket.state() != QAbstractSocket::UnconnectedState)
                m_socket.disconnectFromHost();
            qCWarning(sshclient) << m_name << ": ssh socket connection error";
            emit sshError();
            return;
        }
    }
}
