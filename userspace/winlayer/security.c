/* advapi32's security: SIDs, ACLs, security descriptors, tokens and
 * privileges, the event log, tracing, services and LSA.
 *
 * ICDA keeps no Windows access control: every object may be used by the one
 * user a Windows program runs as.  What programs build (ACLs, descriptors,
 * explicit access lists) is built as Windows lays it out, so that reading it
 * back finds what was written; checks grant what is asked; and the parts of
 * the system ICDA has none of (services, LSA policy, logons) say no. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T n);
HLOCAL WINAPI LocalFree(HLOCAL h);
BOOL WINAPI CreateProcessW(const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env,
                           const WCHAR *dir, void *si, void *pi);

/* the user's SID (S-1-5-21-1001-1002-1003-1000), as misc.c's token gives it */
static const uint8_t user_sid[] = { 1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE9, 3, 0, 0, 0xEA, 3, 0, 0, 0xEB, 3, 0, 0, 0xE8, 3, 0, 0 };

static DWORD sid_len(const uint8_t *sid) { return sid ? 8u + 4u * sid[1] : 0; }

/* ---- SIDs ------------------------------------------------------------------------------------ */

DWORD *WINAPI GetSidSubAuthority(uint8_t *sid, DWORD n) { return (DWORD *)(sid + 8 + 4 * n); }
BYTE *WINAPI GetSidSubAuthorityCount(uint8_t *sid) { return sid + 1; }
void *WINAPI GetSidIdentifierAuthority(uint8_t *sid) { return sid + 2; }

/* "S-1-5-32-544", or the two-letter names SDDL uses for the common ones */
static int parse_sid(const char *s, uint8_t *out) {
    static const struct {
        const char *abbr, *sid;
    } names[] = {
        { "WD", "S-1-1-0" }, { "BA", "S-1-5-32-544" }, { "BU", "S-1-5-32-545" }, { "SY", "S-1-5-18" },
        { "LS", "S-1-5-19" }, { "NS", "S-1-5-20" }, { "AU", "S-1-5-11" }, { "IU", "S-1-5-4" }, { "CO", "S-1-3-0" },
        { "AC", "S-1-15-2-1" }, { "RC", "S-1-5-12" }, { "LW", "S-1-16-4096" }, { "ME", "S-1-16-8192" },
        { "HI", "S-1-16-12288" }, { "SI", "S-1-16-16384" }, { "OW", "S-1-3-4" },
    };
    unsigned long long auth;
    int n = 0;
    const char *p;
    char *e;
    if (s[0] && s[1] && !s[2]) {
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(s, names[i].abbr)) return parse_sid(names[i].sid, out);
        if (!strcmp(s, "OW")) return -1;
    }
    if ((s[0] != 'S' && s[0] != 's') || s[1] != '-') return -1;
    out[0] = (uint8_t)strtoul(s + 2, &e, 10);
    if (*e != '-') return -1;
    auth = strtoull(e + 1, &e, 10);
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(auth >> (8 * (5 - i)));
    for (p = e; *p == '-' && n < 15; n++) {
        uint32_t v = (uint32_t)strtoul(p + 1, &e, 10);
        memcpy(out + 8 + 4 * n, &v, 4);
        p = e;
    }
    if (*p) return -1;
    out[1] = (uint8_t)n;
    return 0;
}

BOOL WINAPI ConvertStringSidToSidW(const WCHAR *s, void **sid) {
    char a[200];
    uint8_t buf[68];
    if (!s || !sid) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    w_to_utf8(s, -1, a, sizeof(a));
    if (parse_sid(a, buf) != 0) {
        wset_last_error(1337);        /* ERROR_INVALID_SID */
        return FALSE;
    }
    *sid = LocalAlloc(0, sid_len(buf));
    memcpy(*sid, buf, sid_len(buf));
    return TRUE;
}

BOOL WINAPI ConvertStringSidToSidA(const char *s, void **sid) {
    WCHAR w[200];
    if (!s) return ConvertStringSidToSidW(0, sid);
    utf8_to_w(s, -1, w, 200);
    return ConvertStringSidToSidW(w, sid);
}

/* WELL_KNOWN_SID_TYPE: the ones programs ask for */
BOOL WINAPI CreateWellKnownSid(int type, const void *domain, void *sid, DWORD *size) {
    const char *s = 0;
    uint8_t buf[68];
    (void)domain;
    switch (type) {
    case 0: s = "S-1-0-0"; break;            /* WinNullSid */
    case 1: s = "S-1-1-0"; break;            /* WinWorldSid */
    case 3: s = "S-1-3-0"; break;            /* WinCreatorOwnerSid */
    case 12: s = "S-1-5-4"; break;           /* WinInteractiveSid */
    case 17: s = "S-1-5-11"; break;          /* WinAuthenticatedUserSid */
    case 18: s = "S-1-5-12"; break;          /* WinRestrictedCodeSid */
    case 22: s = "S-1-5-18"; break;          /* WinLocalSystemSid */
    case 23: s = "S-1-5-19"; break;          /* WinLocalServiceSid */
    case 24: s = "S-1-5-20"; break;          /* WinNetworkServiceSid */
    case 26: s = "S-1-5-32-544"; break;      /* WinBuiltinAdministratorsSid */
    case 27: s = "S-1-5-32-545"; break;      /* WinBuiltinUsersSid */
    case 64: s = "S-1-16-0"; break;          /* WinUntrustedLabelSid */
    case 65: s = "S-1-16-4096"; break;       /* WinLowLabelSid */
    case 66: s = "S-1-16-8192"; break;       /* WinMediumLabelSid */
    case 67: s = "S-1-16-12288"; break;      /* WinHighLabelSid */
    case 68: s = "S-1-16-16384"; break;      /* WinSystemLabelSid */
    case 84: s = "S-1-15-2-1"; break;        /* WinBuiltinAnyPackageSid */
    default: s = "S-1-1-0"; break;
    }
    parse_sid(s, buf);
    if (!size) return FALSE;
    if (*size < sid_len(buf) || !sid) {
        *size = sid_len(buf);
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(sid, buf, sid_len(buf));
    *size = sid_len(buf);
    return TRUE;
}

BOOL WINAPI IsWellKnownSid(const uint8_t *sid, int type) {
    uint8_t buf[68];
    DWORD n = sizeof(buf);
    return sid && CreateWellKnownSid(type, 0, buf, &n) && n == sid_len(sid) && !memcmp(buf, sid, n);
}

/* ---- ACLs ------------------------------------------------------------------------------------ */

typedef struct {
    BYTE rev, sbz1;
    WORD size, count, sbz2;
} ACL;

typedef struct {
    BYTE type, flags;
    WORD size;
} ACE_HEADER;

BOOL WINAPI InitializeAcl(ACL *acl, DWORD size, DWORD rev) {
    if (!acl || size < sizeof(ACL)) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(acl, 0, sizeof(*acl));
    acl->rev = (BYTE)(rev ? rev : 2);
    acl->size = (WORD)(size > 0xFFFC ? 0xFFFC : size);
    return TRUE;
}

static uint8_t *acl_end(ACL *acl) {
    uint8_t *p = (uint8_t *)(acl + 1);
    for (int i = 0; i < acl->count; i++) p += ((ACE_HEADER *)p)->size;
    return p;
}

static BOOL add_ace(ACL *acl, BYTE type, BYTE flags, DWORD mask, const uint8_t *sid) {
    uint8_t *p;
    DWORD need = 8 + sid_len(sid);
    if (!acl || !sid) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    p = acl_end(acl);
    if ((size_t)(p - (uint8_t *)acl) + need > acl->size) {
        wset_last_error(1344);          /* ERROR_ALLOTTED_SPACE_EXCEEDED */
        return FALSE;
    }
    ((ACE_HEADER *)p)->type = type;
    ((ACE_HEADER *)p)->flags = flags;
    ((ACE_HEADER *)p)->size = (WORD)need;
    memcpy(p + 4, &mask, 4);
    memcpy(p + 8, sid, sid_len(sid));
    acl->count++;
    return TRUE;
}

BOOL WINAPI AddAccessAllowedAce(ACL *acl, DWORD rev, DWORD mask, const uint8_t *sid) {
    (void)rev;
    return add_ace(acl, 0, 0, mask, sid);
}
BOOL WINAPI AddAccessAllowedAceEx(ACL *acl, DWORD rev, DWORD flags, DWORD mask, const uint8_t *sid) {
    (void)rev;
    return add_ace(acl, 0, (BYTE)flags, mask, sid);
}
BOOL WINAPI AddAccessDeniedAce(ACL *acl, DWORD rev, DWORD mask, const uint8_t *sid) {
    (void)rev;
    return add_ace(acl, 1, 0, mask, sid);
}
BOOL WINAPI AddAccessDeniedAceEx(ACL *acl, DWORD rev, DWORD flags, DWORD mask, const uint8_t *sid) {
    (void)rev;
    return add_ace(acl, 1, (BYTE)flags, mask, sid);
}
BOOL WINAPI AddMandatoryAce(ACL *acl, DWORD rev, DWORD flags, DWORD policy, const uint8_t *sid) {
    (void)rev;
    return add_ace(acl, 0x11, (BYTE)flags, policy, sid);       /* SYSTEM_MANDATORY_LABEL_ACE_TYPE */
}
BOOL WINAPI AddConditionalAce(ACL *acl, DWORD rev, DWORD flags, BYTE type, DWORD mask, const uint8_t *sid,
                              const WCHAR *cond, DWORD *ret) {
    (void)rev, (void)cond, (void)ret;
    return add_ace(acl, type == 9 ? 0 : type, (BYTE)flags, mask, sid);   /* the condition: not kept */
}

BOOL WINAPI AddAce(ACL *acl, DWORD rev, DWORD start, const void *list, DWORD len) {
    const uint8_t *p = list;
    (void)rev, (void)start;
    while (len >= 4 && ((const ACE_HEADER *)p)->size && ((const ACE_HEADER *)p)->size <= len) {
        const ACE_HEADER *a = (const ACE_HEADER *)p;
        if (!add_ace(acl, a->type, a->flags, *(const DWORD *)(p + 4), p + 8)) return FALSE;
        len -= a->size;
        p += a->size;
    }
    return TRUE;
}

BOOL WINAPI GetAce(ACL *acl, DWORD index, void **ace) {
    uint8_t *p;
    if (!acl || index >= acl->count) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    p = (uint8_t *)(acl + 1);
    for (DWORD i = 0; i < index; i++) p += ((ACE_HEADER *)p)->size;
    *ace = p;
    return TRUE;
}

BOOL WINAPI DeleteAce(ACL *acl, DWORD index) {
    uint8_t *p, *end;
    WORD n;
    if (!acl || index >= acl->count) return FALSE;
    GetAce(acl, index, (void **)&p);
    end = acl_end(acl);
    n = ((ACE_HEADER *)p)->size;
    memmove(p, p + n, (size_t)(end - p - n));
    acl->count--;
    return TRUE;
}

BOOL WINAPI IsValidAcl(const ACL *acl) { return acl != 0 && acl->size >= sizeof(ACL); }

/* AclSizeInformation (2) and AclRevisionInformation (1) */
BOOL WINAPI GetAclInformation(ACL *acl, void *info, DWORD len, int cls) {
    if (!acl || !info) return FALSE;
    if (cls == 2 && len >= 12) {
        DWORD used = (DWORD)(acl_end(acl) - (uint8_t *)acl);
        ((DWORD *)info)[0] = acl->count;
        ((DWORD *)info)[1] = used;
        ((DWORD *)info)[2] = acl->size > used ? acl->size - used : 0;
        return TRUE;
    }
    if (cls == 1 && len >= 4) {
        *(DWORD *)info = acl->rev;
        return TRUE;
    }
    wset_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/* ---- explicit access lists (the Authz-like helpers) ----------------------------------------------- */

typedef struct trustee {
    struct trustee *multiple;
    int             op;
    int             form;       /* 0 TRUSTEE_IS_SID, 1 _IS_NAME, ... */
    int             type;
    void           *name;       /* a SID, or a name */
} TRUSTEE_W;

typedef struct {
    DWORD     perms;
    int       mode;             /* GRANT_ACCESS 1, SET_ACCESS 2, DENY_ACCESS 3, REVOKE_ACCESS 4 */
    DWORD     inherit;
    TRUSTEE_W trustee;
} EXPLICIT_ACCESS_W;

void WINAPI BuildTrusteeWithSidW(TRUSTEE_W *t, void *sid) {
    memset(t, 0, sizeof(*t));
    t->form = 0;
    t->name = sid;
}
void WINAPI BuildTrusteeWithNameW(TRUSTEE_W *t, WCHAR *name) {
    memset(t, 0, sizeof(*t));
    t->form = 1;
    t->name = name;
}
void WINAPI BuildExplicitAccessWithNameW(EXPLICIT_ACCESS_W *e, WCHAR *name, DWORD perms, int mode, DWORD inherit) {
    memset(e, 0, sizeof(*e));
    e->perms = perms;
    e->mode = mode;
    e->inherit = inherit;
    BuildTrusteeWithNameW(&e->trustee, name);
}
/* the A structs are the W ones with UTF-8 names: TRUSTEE_A's name is a char * */
void WINAPI BuildTrusteeWithNameA(TRUSTEE_W *t, char *name) { BuildTrusteeWithNameW(t, (WCHAR *)name); }
void WINAPI BuildExplicitAccessWithNameA(EXPLICIT_ACCESS_W *e, char *name, DWORD perms, int mode, DWORD inherit) {
    BuildExplicitAccessWithNameW(e, (WCHAR *)name, perms, mode, inherit);
}

/* a trustee's SID: its own, or for a name the user's (CURRENT_USER and every
 * account name is the one user here) */
static const uint8_t *trustee_sid(const TRUSTEE_W *t, uint8_t *buf, int narrow) {
    if (t->form == 0 && t->name) return t->name;
    if (t->form == 1 && t->name) {
        char a[80];
        if (narrow) snprintf(a, sizeof(a), "%s", (const char *)t->name);
        else w_to_utf8(t->name, -1, a, sizeof(a));
        if (!strcmp(a, "EVERYONE") || !strcmp(a, "Everyone")) return parse_sid("S-1-1-0", buf) == 0 ? buf : user_sid;
        if (!strcmp(a, "SYSTEM")) return parse_sid("S-1-5-18", buf) == 0 ? buf : user_sid;
        if (!parse_sid(a, buf)) return buf;
    }
    return user_sid;
}

static DWORD set_entries(DWORD n, EXPLICIT_ACCESS_W *e, ACL *old, ACL **out, int narrow) {
    DWORD size = sizeof(ACL);
    ACL *acl;
    if (!out) return ERROR_INVALID_PARAMETER;
    if (old) size += (DWORD)(acl_end(old) - (uint8_t *)(old + 1));
    for (DWORD i = 0; i < n; i++) size += 8 + 68;
    acl = LocalAlloc(0x40, size);
    InitializeAcl(acl, size, 2);
    if (old) AddAce(acl, 2, 0, old + 1, (DWORD)(acl_end(old) - (uint8_t *)(old + 1)));
    for (DWORD i = 0; i < n; i++) {
        uint8_t buf[68];
        const uint8_t *sid = trustee_sid(&e[i].trustee, buf, narrow);
        if (e[i].mode == 4) continue;                                   /* REVOKE_ACCESS */
        add_ace(acl, e[i].mode == 3 ? 1 : 0, (BYTE)e[i].inherit, e[i].perms, sid);
    }
    *out = acl;
    return 0;
}

DWORD WINAPI SetEntriesInAclW(DWORD n, EXPLICIT_ACCESS_W *e, ACL *old, ACL **out) { return set_entries(n, e, old, out, 0); }
DWORD WINAPI SetEntriesInAclA(DWORD n, EXPLICIT_ACCESS_W *e, ACL *old, ACL **out) { return set_entries(n, e, old, out, 1); }

/* ---- security descriptors ------------------------------------------------------------------------- */

typedef struct {
    BYTE  rev, sbz1;
    WORD  control;
    void *owner, *group;
    ACL  *sacl, *dacl;
} SD_ABS;

typedef struct {
    BYTE  rev, sbz1;
    WORD  control;
    DWORD owner, group, sacl, dacl;
} SD_REL;

#define SE_DACL_PRESENT   0x0004
#define SE_SACL_PRESENT   0x0010
#define SE_SELF_RELATIVE  0x8000

BOOL WINAPI InitializeSecurityDescriptor(SD_ABS *sd, DWORD rev) {
    if (!sd) return FALSE;
    memset(sd, 0, sizeof(*sd));
    sd->rev = (BYTE)(rev ? rev : 1);
    return TRUE;
}

static void *sd_part(void *sd, int which) {          /* 0 owner, 1 group, 2 sacl, 3 dacl */
    if (!sd) return 0;
    if (((SD_REL *)sd)->control & SE_SELF_RELATIVE) {
        DWORD off = which == 0 ? ((SD_REL *)sd)->owner : which == 1 ? ((SD_REL *)sd)->group
                  : which == 2 ? ((SD_REL *)sd)->sacl : ((SD_REL *)sd)->dacl;
        return off ? (uint8_t *)sd + off : 0;
    }
    return which == 0 ? ((SD_ABS *)sd)->owner : which == 1 ? ((SD_ABS *)sd)->group
         : which == 2 ? (void *)((SD_ABS *)sd)->sacl : (void *)((SD_ABS *)sd)->dacl;
}

BOOL WINAPI SetSecurityDescriptorDacl(SD_ABS *sd, BOOL present, ACL *dacl, BOOL defaulted) {
    if (!sd || (sd->control & SE_SELF_RELATIVE)) return FALSE;
    sd->control = (WORD)((sd->control & ~(SE_DACL_PRESENT | 0x8)) | (present ? SE_DACL_PRESENT : 0) | (defaulted ? 0x8 : 0));
    sd->dacl = present ? dacl : 0;
    return TRUE;
}
BOOL WINAPI SetSecurityDescriptorSacl(SD_ABS *sd, BOOL present, ACL *sacl, BOOL defaulted) {
    if (!sd || (sd->control & SE_SELF_RELATIVE)) return FALSE;
    sd->control = (WORD)((sd->control & ~(SE_SACL_PRESENT | 0x20)) | (present ? SE_SACL_PRESENT : 0) | (defaulted ? 0x20 : 0));
    sd->sacl = present ? sacl : 0;
    return TRUE;
}
BOOL WINAPI SetSecurityDescriptorOwner(SD_ABS *sd, void *sid, BOOL defaulted) {
    if (!sd || (sd->control & SE_SELF_RELATIVE)) return FALSE;
    sd->owner = sid;
    sd->control = (WORD)((sd->control & ~1) | (defaulted ? 1 : 0));
    return TRUE;
}
BOOL WINAPI SetSecurityDescriptorGroup(SD_ABS *sd, void *sid, BOOL defaulted) {
    if (!sd || (sd->control & SE_SELF_RELATIVE)) return FALSE;
    sd->group = sid;
    sd->control = (WORD)((sd->control & ~2) | (defaulted ? 2 : 0));
    return TRUE;
}
BOOL WINAPI SetSecurityDescriptorControl(SD_ABS *sd, WORD mask, WORD bits) {
    if (!sd) return FALSE;
    sd->control = (WORD)((sd->control & ~mask) | (bits & mask));
    return TRUE;
}

BOOL WINAPI GetSecurityDescriptorDacl(void *sd, BOOL *present, ACL **dacl, BOOL *defaulted) {
    WORD c = sd ? ((SD_REL *)sd)->control : 0;
    if (!sd) return FALSE;
    if (present) *present = (c & SE_DACL_PRESENT) != 0;
    if (dacl) *dacl = sd_part(sd, 3);
    if (defaulted) *defaulted = (c & 0x8) != 0;
    return TRUE;
}
BOOL WINAPI GetSecurityDescriptorSacl(void *sd, BOOL *present, ACL **sacl, BOOL *defaulted) {
    WORD c = sd ? ((SD_REL *)sd)->control : 0;
    if (!sd) return FALSE;
    if (present) *present = (c & SE_SACL_PRESENT) != 0;
    if (sacl) *sacl = sd_part(sd, 2);
    if (defaulted) *defaulted = (c & 0x20) != 0;
    return TRUE;
}
BOOL WINAPI GetSecurityDescriptorOwner(void *sd, void **sid, BOOL *defaulted) {
    if (!sd) return FALSE;
    if (sid) *sid = sd_part(sd, 0);
    if (defaulted) *defaulted = (((SD_REL *)sd)->control & 1) != 0;
    return TRUE;
}
BOOL WINAPI GetSecurityDescriptorGroup(void *sd, void **sid, BOOL *defaulted) {
    if (!sd) return FALSE;
    if (sid) *sid = sd_part(sd, 1);
    if (defaulted) *defaulted = (((SD_REL *)sd)->control & 2) != 0;
    return TRUE;
}
BOOL WINAPI GetSecurityDescriptorControl(void *sd, WORD *control, DWORD *rev) {
    if (!sd) return FALSE;
    if (control) *control = ((SD_REL *)sd)->control;
    if (rev) *rev = ((SD_REL *)sd)->rev;
    return TRUE;
}
BOOL WINAPI IsValidSecurityDescriptor(void *sd) { return sd != 0 && ((SD_REL *)sd)->rev == 1; }

static DWORD acl_len(ACL *a) { return a ? a->size : 0; }

DWORD WINAPI GetSecurityDescriptorLength(void *sd) {
    if (!sd) return 0;
    return sizeof(SD_REL) + sid_len(sd_part(sd, 0)) + sid_len(sd_part(sd, 1)) + acl_len(sd_part(sd, 2)) + acl_len(sd_part(sd, 3));
}

/* a self-relative copy, in one LocalAlloc block */
static void *make_relative(void *owner, void *group, ACL *sacl, ACL *dacl, WORD control, DWORD *size) {
    DWORD n = sizeof(SD_REL) + sid_len(owner) + sid_len(group) + acl_len(sacl) + acl_len(dacl), o = sizeof(SD_REL);
    SD_REL *r = LocalAlloc(0x40, n);
    r->rev = 1;
    r->control = (WORD)(control | SE_SELF_RELATIVE);
    if (owner) memcpy((uint8_t *)r + o, owner, sid_len(owner)), r->owner = o, o += sid_len(owner);
    if (group) memcpy((uint8_t *)r + o, group, sid_len(group)), r->group = o, o += sid_len(group);
    if (sacl) memcpy((uint8_t *)r + o, sacl, acl_len(sacl)), r->sacl = o, o += acl_len(sacl);
    if (dacl) memcpy((uint8_t *)r + o, dacl, acl_len(dacl)), r->dacl = o, o += acl_len(dacl);
    if (size) *size = n;
    return r;
}

BOOL WINAPI MakeSelfRelativeSD(void *abs, void *rel, DWORD *len) {
    DWORD n;
    void *r;
    if (!abs || !len) return FALSE;
    r = make_relative(sd_part(abs, 0), sd_part(abs, 1), sd_part(abs, 2), sd_part(abs, 3),
                      (WORD)(((SD_REL *)abs)->control & ~SE_SELF_RELATIVE), &n);
    if (!rel || *len < n) {
        *len = n;
        LocalFree(r);
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(rel, r, n);
    LocalFree(r);
    return TRUE;
}

/* what an object's security is said to be: owned by the user, a DACL that
 * is present and empty of restrictions (NULL: everyone may) */
static void *object_sd(void) {
    return make_relative((void *)user_sid, (void *)user_sid, 0, 0, SE_DACL_PRESENT, 0);
}

DWORD WINAPI GetSecurityInfo(HANDLE h, int type, DWORD info, void **owner, void **group, ACL **dacl, ACL **sacl, void **sd) {
    void *d = object_sd();
    (void)h, (void)type, (void)info;
    if (owner) *owner = sd_part(d, 0);
    if (group) *group = sd_part(d, 1);
    if (dacl) *dacl = 0;
    if (sacl) *sacl = 0;
    if (sd) *sd = d;
    else if (!owner && !group) LocalFree(d);
    return 0;
}

static DWORD build_sd(TRUSTEE_W *owner, TRUSTEE_W *group, DWORD nacc, EXPLICIT_ACCESS_W *acc, DWORD *size, void **sd,
                      int narrow) {
    ACL *dacl = 0;
    uint8_t ob[68], gb[68];
    if (!sd) return ERROR_INVALID_PARAMETER;
    if (nacc) set_entries(nacc, acc, 0, &dacl, narrow);
    *sd = make_relative(owner ? (void *)trustee_sid(owner, ob, narrow) : 0, group ? (void *)trustee_sid(group, gb, narrow) : 0,
                        0, dacl, SE_DACL_PRESENT, size);
    if (dacl) LocalFree(dacl);
    return 0;
}

DWORD WINAPI BuildSecurityDescriptorW(TRUSTEE_W *owner, TRUSTEE_W *group, DWORD nacc, EXPLICIT_ACCESS_W *acc, DWORD naud,
                                      EXPLICIT_ACCESS_W *aud, void *old, DWORD *size, void **sd) {
    (void)naud, (void)aud, (void)old;
    return build_sd(owner, group, nacc, acc, size, sd, 0);
}

DWORD WINAPI BuildSecurityDescriptorA(TRUSTEE_W *owner, TRUSTEE_W *group, DWORD nacc, EXPLICIT_ACCESS_W *acc, DWORD naud,
                                      EXPLICIT_ACCESS_W *aud, void *old, DWORD *size, void **sd) {
    (void)naud, (void)aud, (void)old;
    return build_sd(owner, group, nacc, acc, size, sd, 1);
}

/* SDDL out: the owner and an allow-all DACL (what object_sd says) */
BOOL WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorW(void *sd, DWORD rev, DWORD info, WCHAR **out, ULONG *len) {
    static const char s[] = "O:S-1-5-21-1001-1002-1003-1000D:(A;;GA;;;WD)";
    (void)sd, (void)rev, (void)info;
    if (!out) return FALSE;
    *out = LocalAlloc(0, sizeof(s) * 2);
    utf8_to_w(s, -1, *out, sizeof(s));
    if (len) *len = sizeof(s);
    return TRUE;
}

/* the same string in UTF-8, in its own LocalAlloc block; *len counts the NUL */
BOOL WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorA(void *sd, DWORD rev, DWORD info, char **out, ULONG *len) {
    WCHAR *w;
    int n;
    if (!out || !ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, rev, info, &w, 0)) return FALSE;
    n = w_to_utf8(w, -1, 0, 0);
    *out = LocalAlloc(0, (SIZE_T)n);
    if (*out) w_to_utf8(w, -1, *out, n);
    LocalFree(w);
    if (!*out) {
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (len) *len = (ULONG)n;
    return TRUE;
}

BOOL WINAPI GetFileSecurityW(const WCHAR *name, DWORD info, void *sd, DWORD len, DWORD *need) {
    DWORD n;
    void *d = make_relative((void *)user_sid, (void *)user_sid, 0, 0, SE_DACL_PRESENT, &n);
    (void)name, (void)info;
    if (need) *need = n;
    if (!sd || len < n) {
        LocalFree(d);
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(sd, d, n);
    LocalFree(d);
    return TRUE;
}

BOOL WINAPI GetFileSecurityA(const char *name, DWORD info, void *sd, DWORD len, DWORD *need) {
    WCHAR *w = utf8_to_w_dup(name);
    BOOL r = GetFileSecurityW(w, info, sd, len, need);
    free(w);
    return r;
}

static BOOL WINAPI SecOk(void) { return TRUE; }
static DWORD WINAPI SecZero(void) { return 0; }

/* ---- access checks and privileges ------------------------------------------------------------------ */

typedef struct {
    DWORD read, write, execute, all;
} GENERIC_MAPPING;

void WINAPI MapGenericMask(DWORD *mask, const GENERIC_MAPPING *m) {
    if (!mask || !m) return;
    if (*mask & 0x80000000u) *mask |= m->read;
    if (*mask & 0x40000000u) *mask |= m->write;
    if (*mask & 0x20000000u) *mask |= m->execute;
    if (*mask & 0x10000000u) *mask |= m->all;
    *mask &= 0x0FFFFFFFu;
}

BOOL WINAPI AccessCheck(void *sd, HANDLE token, DWORD want, const GENERIC_MAPPING *m, void *privs, DWORD *plen, DWORD *granted,
                        BOOL *status) {
    (void)sd, (void)token, (void)privs, (void)plen;
    if (m) MapGenericMask(&want, m);
    if (granted) *granted = want;
    if (status) *status = TRUE;
    return TRUE;
}

BOOL WINAPI AllocateLocallyUniqueId(LUID *luid) {
    static LONG next = 0x10000;
    luid->LowPart = (DWORD)__atomic_add_fetch(&next, 1, __ATOMIC_SEQ_CST);
    luid->HighPart = 0;
    return TRUE;
}

/* the privileges' LUIDs, as Windows numbers them */
static int privilege_value(const char *name) {
    static const char *const names[] = {
        0, 0, "SeCreateTokenPrivilege", "SeAssignPrimaryTokenPrivilege", "SeLockMemoryPrivilege",
        "SeIncreaseQuotaPrivilege", "SeMachineAccountPrivilege", "SeTcbPrivilege", "SeSecurityPrivilege",
        "SeTakeOwnershipPrivilege", "SeLoadDriverPrivilege", "SeSystemProfilePrivilege", "SeSystemtimePrivilege",
        "SeProfileSingleProcessPrivilege", "SeIncreaseBasePriorityPrivilege", "SeCreatePagefilePrivilege",
        "SeCreatePermanentPrivilege", "SeBackupPrivilege", "SeRestorePrivilege", "SeShutdownPrivilege",
        "SeDebugPrivilege", "SeAuditPrivilege", "SeSystemEnvironmentPrivilege", "SeChangeNotifyPrivilege",
        "SeRemoteShutdownPrivilege", "SeUndockPrivilege", "SeSyncAgentPrivilege", "SeEnableDelegationPrivilege",
        "SeManageVolumePrivilege", "SeImpersonatePrivilege", "SeCreateGlobalPrivilege",
        "SeTrustedCredManAccessPrivilege", "SeRelabelPrivilege", "SeIncreaseWorkingSetPrivilege",
        "SeTimeZonePrivilege", "SeCreateSymbolicLinkPrivilege", "SeDelegateSessionUserImpersonatePrivilege",
    };
    for (unsigned i = 2; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i])) return (int)i;
    return -1;
}

BOOL WINAPI LookupPrivilegeValueW(const WCHAR *sys, const WCHAR *name, LUID *luid) {
    char a[80];
    int v;
    (void)sys;
    w_to_utf8(name, -1, a, sizeof(a));
    if ((v = privilege_value(a)) < 0) {
        wset_last_error(1313);           /* ERROR_NO_SUCH_PRIVILEGE */
        return FALSE;
    }
    luid->LowPart = (DWORD)v;
    luid->HighPart = 0;
    return TRUE;
}
BOOL WINAPI LookupPrivilegeValueA(const char *sys, const char *name, LUID *luid) {
    WCHAR w[80];
    utf8_to_w(name, -1, w, 80);
    (void)sys;
    return LookupPrivilegeValueW(0, w, luid);
}

/* every privilege asked for is had: success, with ERROR_SUCCESS (not
 * ERROR_NOT_ALL_ASSIGNED) for those that look */
BOOL WINAPI AdjustTokenPrivileges(HANDLE t, BOOL all, void *newp, DWORD len, void *prev, DWORD *ret) {
    (void)t, (void)all, (void)newp;
    if (prev && len >= 4) *(DWORD *)prev = 0;
    if (ret) *ret = 4;
    wset_last_error(0);
    return TRUE;
}

static int restricted_token = 0x5254;

BOOL WINAPI CreateRestrictedToken(HANDLE t, DWORD flags, DWORD nd, void *dis, DWORD np, void *privs, DWORD nr, void *res,
                                  HANDLE *out) {
    (void)t, (void)flags, (void)nd, (void)dis, (void)np, (void)privs, (void)nr, (void)res;
    *out = h_new(H_OTHER, &restricted_token);
    return *out != 0;
}

BOOL WINAPI CreateProcessAsUserW(HANDLE token, const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inherit, DWORD flags,
                                 void *env, const WCHAR *dir, void *si, void *pi);   /* kernel32.c's */
BOOL WINAPI CreateProcessAsUserA(HANDLE token, const char *app, char *cmd, void *pa, void *ta, BOOL inherit, DWORD flags,
                                 void *env, const char *dir, void *si, void *pi);
BOOL WINAPI CreateProcessWithTokenW(HANDLE token, DWORD logon, const WCHAR *app, WCHAR *cmd, DWORD flags, void *env,
                                    const WCHAR *dir, void *si, void *pi) {
    (void)token, (void)logon;
    return CreateProcessW(app, cmd, 0, 0, FALSE, flags, env, dir, si, pi);
}

/* ---- the event log and tracing: written nowhere ---------------------------------------------------- */

HANDLE WINAPI RegisterEventSourceW(const WCHAR *server, const WCHAR *name) {
    (void)server, (void)name;
    return (HANDLE)(uintptr_t)0x45564C47;
}
HANDLE WINAPI RegisterEventSourceA(const char *server, const char *name) {
    (void)server, (void)name;
    return (HANDLE)(uintptr_t)0x45564C47;
}
ULONG WINAPI RegisterTraceGuidsW(void *cb, void *ctx, const GUID *g, ULONG n, void *regs, const WCHAR *mof, const WCHAR *res,
                                 uint64_t *handle) {
    (void)cb, (void)ctx, (void)g, (void)n, (void)regs, (void)mof, (void)res;
    if (handle) *handle = 1;
    return 0;
}
ULONG WINAPI RegisterTraceGuidsA(void *cb, void *ctx, const GUID *g, ULONG n, void *regs, const char *mof, const char *res,
                                 uint64_t *handle) {
    (void)cb, (void)ctx, (void)g, (void)n, (void)regs, (void)mof, (void)res;
    if (handle) *handle = 1;
    return 0;
}
uint64_t WINAPI GetTraceLoggerHandle(void *buf) {
    (void)buf;
    wset_last_error(ERROR_INVALID_HANDLE);
    return 0;
}

/* ---- services, LSA, logons: ICDA has none --------------------------------------------------------- */

static HANDLE WINAPI NoHandle(void) {
    wset_last_error(5);                  /* ERROR_ACCESS_DENIED */
    return 0;
}
static BOOL WINAPI NoAccess(void) {
    wset_last_error(5);
    return FALSE;
}
static LONG WINAPI LsaDenied(void) { return (LONG)0xC0000022; }   /* STATUS_ACCESS_DENIED */
static BOOL WINAPI NoneMapped(void) {
    wset_last_error(1332);               /* ERROR_NONE_MAPPED */
    return FALSE;
}

ULONG WINAPI LsaNtStatusToWinError(LONG status) {
    switch ((uint32_t)status) {
    case 0: return 0;
    case 0xC0000022: return 5;
    case 0xC0000034: return 2;
    case 0xC0000017: return 8;
    case 0xC000000D: return 87;
    case 0xC0000002: return 1;
    default: return 317;                 /* ERROR_MR_MID_NOT_FOUND */
    }
}

#define X(f) WEXP(f)
const wexport_t sec_exports[] = {
    X(GetSidSubAuthority), X(GetSidSubAuthorityCount), X(GetSidIdentifierAuthority), X(ConvertStringSidToSidW),
    X(ConvertStringSidToSidA), X(CreateWellKnownSid), X(IsWellKnownSid), X(InitializeAcl), X(AddAccessAllowedAce),
    X(AddAccessAllowedAceEx), X(AddAccessDeniedAce), X(AddAccessDeniedAceEx), X(AddMandatoryAce), X(AddConditionalAce),
    X(AddAce), X(GetAce), X(DeleteAce), X(IsValidAcl), X(GetAclInformation), X(BuildTrusteeWithSidW),
    WEXPN("BuildTrusteeWithSidA", BuildTrusteeWithSidW), X(BuildTrusteeWithNameW), X(BuildExplicitAccessWithNameW),
    X(BuildExplicitAccessWithNameA), X(SetEntriesInAclW), X(SetEntriesInAclA), X(InitializeSecurityDescriptor),
    X(SetSecurityDescriptorDacl), X(SetSecurityDescriptorSacl), X(SetSecurityDescriptorOwner), X(SetSecurityDescriptorGroup),
    X(SetSecurityDescriptorControl), X(GetSecurityDescriptorDacl), X(GetSecurityDescriptorSacl),
    X(GetSecurityDescriptorOwner), X(GetSecurityDescriptorGroup), X(GetSecurityDescriptorControl),
    X(IsValidSecurityDescriptor), X(GetSecurityDescriptorLength), X(MakeSelfRelativeSD), X(GetSecurityInfo),
    X(BuildSecurityDescriptorW), X(BuildSecurityDescriptorA), X(ConvertSecurityDescriptorToStringSecurityDescriptorW),
    X(ConvertSecurityDescriptorToStringSecurityDescriptorA), X(GetFileSecurityW), X(GetFileSecurityA), X(BuildTrusteeWithNameA),
    WEXPN("SetFileSecurityW", SecOk), WEXPN("SetFileSecurityA", SecOk), WEXPN("SetKernelObjectSecurity", SecOk),
    WEXPN("SetSecurityInfo", SecZero), WEXPN("SetNamedSecurityInfoW", SecZero), X(MapGenericMask), X(AccessCheck),
    X(AllocateLocallyUniqueId), X(LookupPrivilegeValueW), X(LookupPrivilegeValueA), X(AdjustTokenPrivileges),
    X(CreateRestrictedToken), WEXPN("SetTokenInformation", SecOk), WEXPN("SetThreadToken", SecOk),
    WEXPN("ImpersonateAnonymousToken", SecOk), WEXPN("ImpersonateNamedPipeClient", SecOk),
    WEXPN("ImpersonateLoggedOnUser", SecOk), X(CreateProcessAsUserW), X(CreateProcessAsUserA), X(CreateProcessWithTokenW),
    X(RegisterEventSourceW), X(RegisterEventSourceA), WEXPN("ReportEventW", SecOk), WEXPN("ReportEventA", SecOk),
    WEXPN("DeregisterEventSource", SecOk), X(RegisterTraceGuidsW), X(RegisterTraceGuidsA),
    WEXPN("UnregisterTraceGuids", SecZero), WEXPN("TraceEvent", SecZero), X(GetTraceLoggerHandle),
    WEXPN("GetTraceEnableFlags", SecZero), WEXPN("GetTraceEnableLevel", SecZero), WEXPN("OpenSCManagerW", NoHandle),
    WEXPN("OpenSCManagerA", NoHandle), WEXPN("OpenServiceW", NoHandle), WEXPN("OpenServiceA", NoHandle),
    WEXPN("CloseServiceHandle", SecOk), WEXPN("QueryServiceStatus", NoAccess), WEXPN("QueryServiceStatusEx", NoAccess),
    WEXPN("QueryServiceConfigW", NoAccess), WEXPN("ChangeServiceConfigW", NoAccess), WEXPN("StartServiceW", NoAccess),
    WEXPN("QueryServiceConfigA", NoAccess), WEXPN("ChangeServiceConfigA", NoAccess), WEXPN("StartServiceA", NoAccess),
    WEXPN("LsaOpenPolicy", LsaDenied), WEXPN("LsaClose", SecZero), WEXP(LsaNtStatusToWinError),
    WEXPN("LsaAddAccountRights", LsaDenied), WEXPN("LsaFreeMemory", SecZero), WEXPN("LookupAccountNameW", NoneMapped),
    WEXPN("LookupAccountNameA", NoneMapped), WEXPN("LogonUserW", NoAccess), WEXPN("LogonUserA", NoAccess), WEXPN("RegDisablePredefinedCache", SecZero),
};
const int sec_nexports = sizeof(sec_exports) / sizeof(sec_exports[0]);
