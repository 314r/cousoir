#include "calibre_lock.h"

#include <QByteArray>

#include <atomic>

#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#endif

namespace {

std::atomic<int> locksHeld{ 0 };

} // namespace

CalibreLock::~CalibreLock()
{
    release();
}

bool CalibreLock::acquire()
{
    if (m_socket >= 0)
        return true;
#ifdef Q_OS_LINUX
    // create_single_instance_mutex('db') in utils/lock.py: an abstract socket
    // only one process can bind.
    const QByteArray name = "calibre-singleinstance-" + QByteArray::number(::getuid()) + "-db";
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name.constData(), size_t(name.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    const socklen_t length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + size_t(name.size()));
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), length) != 0) {
        ::close(fd);
        return false;
    }
    m_socket = fd;
    ++locksHeld;
    return true;
#else
    m_socket = 0;
    ++locksHeld;
    return true;
#endif
}

void CalibreLock::release()
{
    if (m_socket < 0)
        return;
#ifdef Q_OS_LINUX
    ::close(m_socket);
#endif
    m_socket = -1;
    --locksHeld;
}

bool CalibreLock::heldHere()
{
    return locksHeld > 0;
}

bool CalibreLock::held() const
{
    return m_socket >= 0;
}
