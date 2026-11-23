// Narrow POSIX compatibility for the libnx/newlib Switch target.
// Horizon's libc has lseek/read/write but not positional I/O, getrusage or
// sysconf. Texture-store positional operations are serialized and restore the
// descriptor offset; diagnostics degrade to unavailable CPU-time statistics.
#include <cerrno>
#include <cstddef>
#include <mutex>
#include <sys/types.h>
#include <sys/resource.h>
#include <unistd.h>

namespace
{
    std::mutex g_positionedIoMutex;
}

extern "C" ssize_t pread(int fd, void* buffer, size_t count, off_t offset)
{
    std::lock_guard<std::mutex> lock(g_positionedIoMutex);
    const off_t previous = lseek(fd, 0, SEEK_CUR);
    if (previous == static_cast<off_t>(-1)) return -1;
    if (lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) return -1;
    const ssize_t result = read(fd, buffer, count);
    const int savedErrno = errno;
    (void)lseek(fd, previous, SEEK_SET);
    errno = savedErrno;
    return result;
}

extern "C" ssize_t pwrite(int fd, const void* buffer, size_t count, off_t offset)
{
    std::lock_guard<std::mutex> lock(g_positionedIoMutex);
    const off_t previous = lseek(fd, 0, SEEK_CUR);
    if (previous == static_cast<off_t>(-1)) return -1;
    if (lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) return -1;
    const ssize_t result = write(fd, buffer, count);
    const int savedErrno = errno;
    (void)lseek(fd, previous, SEEK_SET);
    errno = savedErrno;
    return result;
}

extern "C" long sysconf(int name)
{
#ifdef _SC_NPROCESSORS_ONLN
    if (name == _SC_NPROCESSORS_ONLN) return 4;
#endif
#ifdef _SC_NPROCESSORS_CONF
    if (name == _SC_NPROCESSORS_CONF) return 4;
#endif
    errno = EINVAL;
    return -1;
}

extern "C" int getrusage(int, struct rusage* usage)
{
    if (!usage) { errno = EFAULT; return -1; }
    *usage = {};
    errno = ENOSYS;
    return -1;
}
