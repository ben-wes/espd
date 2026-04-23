#include <unistd.h>
#include <sys/types.h>

/*
 * Pure Data's large-file path may reference lseek64 on POSIX-like targets.
 * ESP-IDF/picolibc exposes lseek, so provide a local compatibility symbol.
 */
long long lseek64(int fd, long long offset, int whence)
{
    return (long long)lseek(fd, (off_t)offset, whence);
}
