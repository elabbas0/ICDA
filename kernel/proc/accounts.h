#ifndef ACCOUNTS_H
#define ACCOUNTS_H

#include <stdint.h>

/* User accounts (/etc/icda/users, readable only by the kernel).
 *
 * The first account is the machine's owner: an administrator whose home is
 * the original /home, so an installed system keeps its files and an older
 * kernel still finds them.  Later accounts live in /users/<name>; each user
 * sees their own home as /home.  Passwords are PBKDF2-HMAC-SHA256 with a
 * per-account salt, checked only here. */

#define ACCOUNTS_OWNER_UID 1000

/* SYS_ACCOUNT operations (rdi); strings are user pointers */
enum {
    ACCT_LIST = 1,      /* buf, cap -> "name uid flags\n" per account (a admin, l automatic, p password) */
    ACCT_WHOAMI,        /* buf, cap -> "name uid flags\n", or "" when nobody is signed in */
    ACCT_LOGIN,         /* name, password (0: automatic sign-in) -> 0; only before anyone signed in */
    ACCT_VERIFY,        /* password -> 0 when it is the caller's (the lock screen) */
    ACCT_CREATE,        /* name, password, flags, caller's password -> uid */
    ACCT_DELETE,        /* name, caller's password, remove files -> 0 */
    ACCT_PASSWORD,      /* name, caller's password, new password -> 0 */
    ACCT_SET,           /* name, caller's password, flags -> 0 */
    ACCT_LOGOUT         /* ends the caller's other programs -> 0 */
};

#define ACCT_FLAG_ADMIN     1
#define ACCT_FLAG_AUTOLOGIN 2

void     accounts_init(void);          /* after the file system is up: hooks the VFS */
int64_t  accounts_syscall(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
int      accounts_caller_is_admin(void);   /* the system, or a signed-in administrator */
const char *accounts_passwd_text(void);    /* /etc/passwd for Linux programs */

#endif
