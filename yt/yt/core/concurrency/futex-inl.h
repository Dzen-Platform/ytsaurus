#ifdef _linux_
    #include <linux/futex.h>
    #include <sys/time.h>
    #include <sys/syscall.h>
#endif

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

#ifdef _linux_

namespace NDetail {

inline int futex(
    int* uaddr, int op, int val, const timespec* timeout,
    int* uaddr2, int val3)
{
    return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

} // namespace NDetail

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
