/* src/util/errno_str.c — POSIX errno-to-string accessor.
 *
 * Implements the surface declared in include/util/errno_str.h:
 *   - n00b_errno_str  (pure lookup from int errno value to
 *                      rich-literal n00b_string_t *)
 *
 * The accessor mirrors the project-internal `n00b_attest_err_str`
 * / `n00b_base64_err_str` shape: a `switch` over integer codes,
 * each case returning a rich-string literal (`r"..."`) with
 * process-lifetime storage. Unknown codes (including platform-
 * only extensions not in the portable POSIX.1-2008 set) return a
 * documented fallback.
 *
 * # Coverage rationale
 *
 * Every POSIX.1-2008 portable errno value is in the table. The
 * set was assembled by walking the IEEE Std 1003.1-2008
 * "<errno.h>" page (the normative reference) and cross-checking
 * against the macOS `sys/errno.h` and Linux `asm-generic/errno*.h`
 * headers. Codes that are defined on both platforms with
 * identical semantics are covered; codes that are platform-
 * specific (Linux: `ECHRNG`, `ENONET`, ...; macOS: `ENOATTR`,
 * `EBADRPC`, ...) are deliberately NOT covered — they fall
 * through to the unknown-code fallback, since the accessor cannot
 * promise a stable description across platforms for codes whose
 * very existence is platform-specific.
 *
 * # Sign handling
 *
 * libn00b's `n00b_check_posix` wrapper returns `Err(errno)`
 * (positive) directly per `include/adt/result.h`, but several
 * subsystems re-encode the POSIX code as `Err(-errno)` to avoid
 * collision with negative module-domain codes (e.g., a -1001
 * Statement code would collide with a positive `EPERM = 1`
 * encoded as `-1` only via this negation convention). The
 * accessor folds the sign so a single entry point serves both
 * conventions; `n00b_errno_str(EPERM)` and `n00b_errno_str(-EPERM)`
 * both return the same description.
 *
 * # EAGAIN / EWOULDBLOCK aliasing
 *
 * POSIX permits these to be the same value or distinct. On both
 * Linux and macOS they are identical (`EWOULDBLOCK == EAGAIN`).
 * To stay compilable on the rare platform where they differ, the
 * `EWOULDBLOCK` case is guarded by `#if EWOULDBLOCK != EAGAIN`.
 * On platforms where they are identical the compiler would error
 * on a duplicate `case` label, hence the guard.
 *
 * # ENOTSUP / EOPNOTSUPP aliasing
 *
 * Same situation as EAGAIN / EWOULDBLOCK. On Linux these are
 * identical; on macOS they are distinct (ENOTSUP=45, EOPNOTSUPP=102).
 * Same guard pattern.
 *
 * Implementation note: we use a `switch` against the standard
 * `<errno.h>` macros rather than a numeric table because the codes
 * are not contiguous and differ between platforms — the compiler-
 * supplied `<errno.h>` definitions are authoritative for the host.
 */

#include "n00b.h"
#include "core/string.h"
#include "util/errno_str.h"

#include <errno.h>

n00b_string_t *
n00b_errno_str(int errno_val)
{
    // Fold the sign — libn00b's `n00b_check_posix` returns positive
    // errno but some subsystems re-encode as -errno; one accessor
    // serves both conventions.
    if (errno_val < 0) {
        errno_val = -errno_val;
    }

    switch (errno_val) {
    case 0:
        return r"no error";

    // ---- POSIX.1-2008 portable core ----------------------------
    case EPERM:
        return r"operation not permitted";
    case ENOENT:
        return r"no such file or directory";
    case ESRCH:
        return r"no such process";
    case EINTR:
        return r"interrupted system call";
    case EIO:
        return r"i/o error";
    case ENXIO:
        return r"no such device or address";
    case E2BIG:
        return r"argument list too long";
    case ENOEXEC:
        return r"exec format error";
    case EBADF:
        return r"bad file descriptor";
    case ECHILD:
        return r"no child processes";
    case EAGAIN:
        return r"resource temporarily unavailable";
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
        return r"operation would block";
#endif
    case ENOMEM:
        return r"out of memory";
    case EACCES:
        return r"permission denied";
    case EFAULT:
        return r"bad address";
#if defined(ENOTBLK)
    case ENOTBLK:
        return r"block device required";
#endif
    case EBUSY:
        return r"device or resource busy";
    case EEXIST:
        return r"file already exists";
    case EXDEV:
        return r"cross-device link";
    case ENODEV:
        return r"no such device";
    case ENOTDIR:
        return r"not a directory";
    case EISDIR:
        return r"is a directory";
    case EINVAL:
        return r"invalid argument";
    case ENFILE:
        return r"too many open files in system";
    case EMFILE:
        return r"too many open files in process";
    case ENOTTY:
        return r"not a terminal";
    case ETXTBSY:
        return r"text file busy";
    case EFBIG:
        return r"file too large";
    case ENOSPC:
        return r"no space left on device";
    case ESPIPE:
        return r"illegal seek";
    case EROFS:
        return r"read-only filesystem";
    case EMLINK:
        return r"too many links";
    case EPIPE:
        return r"broken pipe";
    case EDOM:
        return r"math argument out of domain";
    case ERANGE:
        return r"math result out of range";
    case EDEADLK:
        return r"resource deadlock avoided";
    case ENAMETOOLONG:
        return r"file name too long";
    case ENOLCK:
        return r"no record locks available";
    case ENOSYS:
        return r"function not implemented";
    case ENOTEMPTY:
        return r"directory not empty";
    case ELOOP:
        return r"too many levels of symbolic links";
    case ENOMSG:
        return r"no message of desired type";
    case EIDRM:
        return r"identifier removed";

    // ---- POSIX.1-2008 networking -------------------------------
    case ENOSTR:
        return r"device not a stream";
    case ENODATA:
        return r"no data available";
    case ETIME:
        return r"timer expired";
    case ENOSR:
        return r"out of stream resources";
    case ENOLINK:
        return r"link has been severed";
    case EPROTO:
        return r"protocol error";
    case EMULTIHOP:
        return r"multihop attempted";
    case EBADMSG:
        return r"bad message";
    case EOVERFLOW:
        return r"value too large for data type";
    case EILSEQ:
        return r"illegal byte sequence";

    case ENOTSOCK:
        return r"not a socket";
    case EDESTADDRREQ:
        return r"destination address required";
    case EMSGSIZE:
        return r"message too long";
    case EPROTOTYPE:
        return r"protocol wrong type for socket";
    case ENOPROTOOPT:
        return r"protocol not available";
    case EPROTONOSUPPORT:
        return r"protocol not supported";
    case ENOTSUP:
        return r"operation not supported";
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
        return r"operation not supported on socket";
#endif
    case EAFNOSUPPORT:
        return r"address family not supported";
    case EADDRINUSE:
        return r"address already in use";
    case EADDRNOTAVAIL:
        return r"address not available";
    case ENETDOWN:
        return r"network is down";
    case ENETUNREACH:
        return r"network unreachable";
    case ENETRESET:
        return r"network dropped connection on reset";
    case ECONNABORTED:
        return r"connection aborted";
    case ECONNRESET:
        return r"connection reset by peer";
    case ENOBUFS:
        return r"no buffer space available";
    case EISCONN:
        return r"socket already connected";
    case ENOTCONN:
        return r"socket not connected";
    case ETIMEDOUT:
        return r"connection timed out";
    case ECONNREFUSED:
        return r"connection refused";
    case EHOSTUNREACH:
        return r"host unreachable";
    case EALREADY:
        return r"operation already in progress";
    case EINPROGRESS:
        return r"operation in progress";
    case ESTALE:
        return r"stale file handle";
    case EDQUOT:
        return r"disk quota exceeded";
    case ECANCELED:
        return r"operation canceled";
    case EOWNERDEAD:
        return r"owner died";
    case ENOTRECOVERABLE:
        return r"state not recoverable";

    default:
        return r"unknown errno value";
    }
}
