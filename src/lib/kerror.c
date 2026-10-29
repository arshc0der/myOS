/* kerror.c - error code names. */
#include "kerror.h"

const char *kstrerror(int err)
{
    if (err < 0)
        err = -err;
    switch (err) {
    case 0:            return "Success";
    case EPERM:        return "Operation not permitted";
    case ENOENT:       return "No such file or directory";
    case ESRCH:        return "No such task";
    case EIO:          return "I/O error";
    case EBADF:        return "Bad file descriptor";
    case EAGAIN:       return "Try again";
    case ENOMEM:       return "Out of memory";
    case EACCES:       return "Permission denied";
    case EFAULT:       return "Bad address";
    case EBUSY:        return "Resource busy";
    case EEXIST:       return "Already exists";
    case ENODEV:       return "No such device";
    case ENOTDIR:      return "Not a directory";
    case EISDIR:       return "Is a directory";
    case EINVAL:       return "Invalid argument";
    case EMFILE:       return "Too many open files";
    case EFBIG:        return "File too large";
    case ENOSPC:       return "No space left on device";
    case ESPIPE:       return "Illegal seek";
    case ERANGE:       return "Result out of range";
    case ENAMETOOLONG: return "Name too long";
    case ENOSYS:       return "Not implemented";
    case ENOTEMPTY:    return "Directory not empty";
    case ETIMEDOUT:    return "Timed out";
    default:           return "Unknown error";
    }
}
