/*
 * kerror.h - error codes. Kernel functions return a negative error code
 * (for example -ENOENT) when something goes wrong, and 0 or a positive
 * value on success.
 */
#ifndef KERROR_H
#define KERROR_H

#define EPERM         1     /* operation not permitted      */
#define ENOENT        2     /* no such file or directory    */
#define ESRCH         3     /* no such task                 */
#define EIO           5     /* input/output error           */
#define EBADF         9     /* bad file descriptor          */
#define EAGAIN       11     /* try again                    */
#define ENOMEM       12     /* out of memory                */
#define EACCES       13     /* permission denied            */
#define EFAULT       14     /* bad address                  */
#define EBUSY        16     /* resource busy                */
#define EEXIST       17     /* already exists               */
#define ENODEV       19     /* no such device               */
#define ENOTDIR      20     /* not a directory              */
#define EISDIR       21     /* is a directory               */
#define EINVAL       22     /* invalid argument             */
#define EMFILE       24     /* too many open files          */
#define EFBIG        27     /* file too large               */
#define ENOSPC       28     /* no space left on device      */
#define ESPIPE       29     /* illegal seek                 */
#define ERANGE       34     /* result out of range          */
#define ENAMETOOLONG 36     /* name too long                */
#define ENOSYS       38     /* function not implemented     */
#define ENOTEMPTY    39     /* directory not empty          */
#define ETIMEDOUT    110    /* operation timed out          */

/* Text for an error code, e.g. kstrerror(-ENOENT) = "No such file". */
const char *kstrerror(int err);

#endif
