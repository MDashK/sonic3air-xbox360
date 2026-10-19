/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/*
 * Directory enumeration + extended path ops for picolibc on the Xbox 360 kernel,
 * ported from the original Xbox's libs/libc/xbox/dirio.c. This is the POSIX
 * surface libc++'s <filesystem> needs beyond fileio.c: <dirent.h>
 * (opendir/readdir/closedir) over NtQueryDirectoryFile, plus rename/truncate/
 * getcwd/chdir/realpath/lstat/statvfs/openat/unlinkat. Symlinks, hard links and
 * permissions do not exist on FATX, so those report ENOSYS or succeed as no-ops
 * honestly. The fd-based ops reuse fileio.c's descriptor table via
 * __rxdk_fd_handle/__rxdk_fd_install. Kernel-only (Nt/Rtl), never libxapi.
 */

#define _GNU_SOURCE 1  /* openat family, AT_* flags, statvfs, readlink under strict -std */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- NT kernel ABI (matches fileio.c) ------------------------------------- */

typedef long           NTSTATUS;
typedef void          *HANDLE;
typedef unsigned long  ULONG;
typedef unsigned short USHORT;

typedef struct { USHORT Length, MaximumLength; char *Buffer; } ANSI_STRING;
typedef struct { union { NTSTATUS Status; void *Pointer; }; unsigned long Information; } IO_STATUS_BLOCK;
typedef struct { HANDLE RootDirectory; ANSI_STRING *ObjectName; ULONG Attributes; } OBJECT_ATTRIBUTES;
typedef union { long long QuadPart; } LARGE_INTEGER;
typedef struct {
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength;
    char FileName[1];
} FILE_DIRECTORY_INFORMATION;
typedef struct { unsigned char ReplaceIfExists; HANDLE RootDirectory; ANSI_STRING FileName; } FILE_RENAME_INFORMATION;
typedef struct { LARGE_INTEGER EndOfFile; } FILE_END_OF_FILE_INFORMATION;

extern void     RtlInitAnsiString(ANSI_STRING *dst, const char *src);
extern NTSTATUS NtCreateFile(HANDLE *handle, ULONG access, OBJECT_ATTRIBUTES *obja,
                             IO_STATUS_BLOCK *iosb, LARGE_INTEGER *alloc,
                             ULONG attrs, ULONG share, ULONG disp, ULONG options);
extern NTSTATUS NtClose(HANDLE h);
extern NTSTATUS NtSetInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *info,
                                     ULONG len, int cls);
/* NB: the Xbox 360 NtQueryDirectoryFile has NO FileInformationClass parameter
   (unlike desktop NT and the original Xbox); it always returns
   FILE_DIRECTORY_INFORMATION. The arg order is handle, event, apc, apcctx,
   iosb, info, length, FileName, RestartScan. */
extern NTSTATUS NtQueryDirectoryFile(HANDLE h, HANDLE ev, void *apc, void *apcctx,
                                     IO_STATUS_BLOCK *iosb, void *info, ULONG len,
                                     ANSI_STRING *name, unsigned char restart);

/* shared fd table (fileio.c) + the plain file ops we build on */
extern void *__rxdk_fd_handle(int fd);
extern int   __rxdk_fd_install(void *h);
extern int   open(const char *path, int flags, ...);
extern int   close(int fd);
extern int   unlink(const char *path);
extern int   rmdir(const char *path);
extern int   stat(const char *path, struct stat *st);

#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define NT_GENERIC_READ  0x80000000UL
#define NT_GENERIC_WRITE 0x40000000UL
#define NT_SYNCHRONIZE   0x00100000UL
#define NT_DELETE        0x00010000UL
#define NT_LIST_DIRECTORY 0x00000001UL
#define NT_SHARE_RWD     0x00000007UL
#define NT_FILE_OPEN     1UL
#define NT_DIRECTORY_FILE 0x00000001UL
#define NT_SYNCHRONOUS_IO 0x00000020UL
#define NT_ATTR_NORMAL   0x00000080UL
#define OBJ_CASE_INSENSITIVE 0x40UL
#define FILE_DIRECTORY_INFO   1
#define FILE_RENAME_INFO      10
#define FILE_END_OF_FILE_INFO 20
#define DIR_ATTR_DIRECTORY 0x10UL

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* Translate '/'->'\', apply the libc cwd to relative paths, and collapse ./.. --
   shared with fileio.c, defined in pathres.c (which also has getcwd/chdir). */
extern const char *__rxdk_nt_path(const char *in, char *out, size_t n);
extern const char *__rxdk_resolve_path(const char *in, char *out, size_t n);

static NTSTATUS nt_open(const char *path, ULONG access, ULONG disp,
                        ULONG options, HANDLE *out) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES obja;
    IO_STATUS_BLOCK iosb;
    char buf[PATH_MAX];
/* S3AIR X360 PATCH (2026-10-17): open paths the way the XDK's CreateFile does -- the drive form
   ("game:\x") relative to the DOS-devices directory (RootDirectory = (HANDLE)-3) -- instead of
   "\??\game:\x" with no root. The console accepts both; Xenia only resolves the XDK form, so
   with upstream's form fopen/stat/opendir all fail under Xenia. See x360/README.md. */
    RtlInitAnsiString(&name, __rxdk_resolve_path(path, buf, sizeof buf));
    obja.RootDirectory = (HANDLE)(long)-3;
    obja.ObjectName = &name;
    obja.Attributes = OBJ_CASE_INSENSITIVE;
    return NtCreateFile(out, access | NT_SYNCHRONIZE, &obja, &iosb, NULL,
                        NT_ATTR_NORMAL, NT_SHARE_RWD, disp,
                        options | NT_SYNCHRONOUS_IO);
}

/* ---- directory enumeration ------------------------------------------------ */

/* telldir/seekdir position, tracked per open directory. Our readdir enumerates
   with a restart-then-continue kernel query and the (picolibc) DIR struct has no
   spare field, so the logical entry index is kept in a side table keyed by the
   descriptor (opendir installs one in the shared fd table, range [3,32)). */
#define RXDK_DIRPOS_MAX 32
static long g_dirpos[RXDK_DIRPOS_MAX];

static void dirpos_reset(int fd) {
    if (fd >= 0 && fd < RXDK_DIRPOS_MAX) g_dirpos[fd] = 0;
}

DIR *opendir(const char *path) {
    HANDLE h;
    int fd;
    DIR *d;
    if (!NT_SUCCESS(nt_open(path, NT_GENERIC_READ | NT_LIST_DIRECTORY,
                            NT_FILE_OPEN, NT_DIRECTORY_FILE, &h))) {
        errno = ENOENT; return NULL;
    }
    fd = __rxdk_fd_install(h);
    if (fd < 0) { NtClose(h); errno = EMFILE; return NULL; }
    d = (DIR *)malloc(sizeof(DIR));
    if (!d) { close(fd); errno = ENOMEM; return NULL; }
    d->fd = fd; d->offset = 0; d->count = 0;
    dirpos_reset(fd);
    return d;
}

DIR *fdopendir(int fd) {
    DIR *d;
    if (!__rxdk_fd_handle(fd)) { errno = EBADF; return NULL; }
    d = (DIR *)malloc(sizeof(DIR));
    if (!d) { errno = ENOMEM; return NULL; }
    d->fd = fd; d->offset = 0; d->count = 0;
    dirpos_reset(fd);
    return d;
}

struct dirent *readdir(DIR *d) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    FILE_DIRECTORY_INFORMATION *fdi;
    ULONG nlen;
    size_t cap = sizeof(d->dirent.d_name) - 1;

    if (!d || !(h = __rxdk_fd_handle(d->fd))) { errno = EBADF; return NULL; }

    if (d->offset >= d->count) {   /* batch exhausted -> fetch next */
        /* The search spec is supplied as FindFirstFile does: "*" with
           RestartScan on the very first query (d->count still 0), then an
           EMPTY (never NULL) spec with RestartScan clear to continue where the
           enumeration left off. A NULL spec pointer is legal on NT but faults
           xenia (it dereferences the guest-null X_ANSI_STRING), and a non-empty
           spec makes xenia restart every call -- so the empty-spec continuation
           is what both agree on. */
        int is_first = (d->count == 0);
        ANSI_STRING spec;
        RtlInitAnsiString(&spec, is_first ? "*" : "");
        if (!NT_SUCCESS(NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, d->buf,
                                             sizeof d->buf,
                                             &spec, (unsigned char)is_first))) {
            if (is_first) d->count = 0;   /* stay "not started" for rewinddir */
            return NULL;                  /* STATUS_NO_MORE_FILES / error -> end */
        }
        d->count = (size_t)iosb.Information;
        d->offset = 0;
        if (d->count == 0) return NULL;
    }

    fdi = (FILE_DIRECTORY_INFORMATION *)(d->buf + d->offset);
    nlen = fdi->FileNameLength;    /* bytes; FATX names are ANSI */
    if (nlen > cap) nlen = (ULONG)cap;
    memcpy(d->dirent.d_name, fdi->FileName, nlen);
    d->dirent.d_name[nlen] = '\0';
    d->dirent.d_ino = 0;
    d->dirent.d_type = (fdi->FileAttributes & DIR_ATTR_DIRECTORY) ? DT_DIR : DT_REG;

    if (fdi->NextEntryOffset) d->offset += fdi->NextEntryOffset;
    else                      d->offset = d->count;
    if (d->fd >= 0 && d->fd < RXDK_DIRPOS_MAX) g_dirpos[d->fd]++;
    return &d->dirent;
}

int closedir(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    close(d->fd);
    free(d);
    return 0;
}

int dirfd(DIR *d) { if (!d) { errno = EINVAL; return -1; } return d->fd; }
void rewinddir(DIR *d) { if (d) { d->offset = 0; d->count = 0; dirpos_reset(d->fd); } }

/* telldir returns the index of the entry readdir will return next; seekdir
   restores it by rewinding and re-reading (our enumeration has no cheaper
   absolute-seek, but this is exact). */
long telldir(DIR *d) {
    if (!d || d->fd < 0 || d->fd >= RXDK_DIRPOS_MAX) { errno = EBADF; return -1; }
    return g_dirpos[d->fd];
}

void seekdir(DIR *d, long loc) {
    if (!d || loc < 0 || d->fd < 0 || d->fd >= RXDK_DIRPOS_MAX) return;
    rewinddir(d);
    while (g_dirpos[d->fd] < loc && readdir(d) != NULL)
        ;                               /* readdir advances g_dirpos */
}

/* ---- rename / truncate ---------------------------------------------------- */

int rename(const char *oldp, const char *newp) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    unsigned char info[sizeof(FILE_RENAME_INFORMATION) + PATH_MAX];
    FILE_RENAME_INFORMATION *ri = (FILE_RENAME_INFORMATION *)info;
    char nb[PATH_MAX];

    if (!NT_SUCCESS(nt_open(oldp, NT_DELETE | NT_SYNCHRONIZE, NT_FILE_OPEN, 0, &h))) {
        errno = ENOENT; return -1;
    }
    ri->ReplaceIfExists = 1;
    ri->RootDirectory = NULL;
    RtlInitAnsiString(&ri->FileName, __rxdk_nt_path(newp, nb, sizeof nb));
    if (!NT_SUCCESS(NtSetInformationFile(h, &iosb, ri, sizeof info, FILE_RENAME_INFO))) {
        NtClose(h); errno = EIO; return -1;
    }
    NtClose(h);
    return 0;
}

int ftruncate(int fd, off_t length) {
    HANDLE h = __rxdk_fd_handle(fd);
    IO_STATUS_BLOCK iosb;
    FILE_END_OF_FILE_INFORMATION eof;
    if (!h) { errno = EBADF; return -1; }
    eof.EndOfFile.QuadPart = length;
    if (!NT_SUCCESS(NtSetInformationFile(h, &iosb, &eof, sizeof eof, FILE_END_OF_FILE_INFO))) {
        errno = EIO; return -1;
    }
    return 0;
}

int truncate(const char *path, off_t length) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    FILE_END_OF_FILE_INFORMATION eof;
    if (!NT_SUCCESS(nt_open(path, NT_GENERIC_WRITE, NT_FILE_OPEN, 0, &h))) {
        errno = ENOENT; return -1;
    }
    eof.EndOfFile.QuadPart = length;
    if (!NT_SUCCESS(NtSetInformationFile(h, &iosb, &eof, sizeof eof, FILE_END_OF_FILE_INFO))) {
        NtClose(h); errno = EIO; return -1;
    }
    NtClose(h);
    return 0;
}

/* ---- stat variants -------------------------------------------------------- */
/* getcwd/chdir/realpath and the cwd itself live in pathres.c (shared resolver). */

int lstat(const char *path, struct stat *st) { return stat(path, st); }  /* no symlinks */

/* ---- statvfs (volume free/total, best-effort zero) ------------------------ */

int statvfs(const char *path, struct statvfs *b) {
    (void)path;
    if (!b) { errno = EINVAL; return -1; }
    memset(b, 0, sizeof *b);
    b->f_bsize = b->f_frsize = 4096;   /* FATX cluster-ish; sizes unknown here */
    return 0;
}

/* ---- *at() ops (only AT_FDCWD; the 360 has no dirfd-relative open) --------- */

int openat(int dirfd, const char *path, int flags, ...) {
    if (dirfd == AT_FDCWD) return open(path, flags);
    errno = ENOSYS; return -1;
}
int unlinkat(int dirfd, const char *path, int flag) {
    if (dirfd != AT_FDCWD) { errno = ENOSYS; return -1; }
    return (flag & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

/* The rest of the *at family: AT_FDCWD resolves against the libc cwd (the plain
   call already does), and there is no other dirfd-relative open on the 360. */
extern int access(const char *path, int amode);

int mkdirat(int dirfd, const char *path, mode_t mode) {
    if (dirfd != AT_FDCWD) { errno = ENOSYS; return -1; }
    return mkdir(path, mode);
}
int fstatat(int dirfd, const char *path, struct stat *st, int flag) {
    (void)flag;
    if (dirfd != AT_FDCWD) { errno = ENOSYS; return -1; }
    return stat(path, st);
}
int faccessat(int dirfd, const char *path, int amode, int flag) {
    (void)flag;
    if (dirfd != AT_FDCWD) { errno = ENOSYS; return -1; }
    return access(path, amode);
}
int fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flag) {
    (void)path; (void)owner; (void)group; (void)flag;
    if (dirfd != AT_FDCWD) { errno = ENOSYS; return -1; }
    return 0;                              /* FATX/GDFX have no ownership */
}
int futimens(int fd, const struct timespec times[2]) {
    (void)fd; (void)times; return 0;       /* timestamps not settable via fd */
}
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flag) {
    (void)path; (void)times; (void)flag;
    if (dirfd != AT_FDCWD) { errno = ENOSYS; return -1; }
    return 0;
}
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsize) {
    (void)dirfd; (void)path; (void)buf; (void)bufsize;
    errno = EINVAL; return -1;             /* no symlinks */
}
int symlinkat(const char *target, int dirfd, const char *linkpath) {
    (void)target; (void)dirfd; (void)linkpath; errno = ENOSYS; return -1;
}
int linkat(int ofd, const char *oldp, int nfd, const char *newp, int flag) {
    (void)ofd; (void)oldp; (void)nfd; (void)newp; (void)flag;
    errno = ENOSYS; return -1;             /* no hard links */
}
int fchdir(int fd) {
    (void)fd; errno = ENOSYS; return -1;   /* no fd->path mapping to chdir into */
}
int renameat(int ofd, const char *oldp, int nfd, const char *newp) {
    if (ofd != AT_FDCWD || nfd != AT_FDCWD) { errno = ENOSYS; return -1; }
    return rename(oldp, newp);
}

/* ---- unsupported on FATX: honest failures / no-ops ------------------------ */

int fchmod(int fd, mode_t mode) { (void)fd; (void)mode; return 0; }   /* no perms */
int fchmodat(int dirfd, const char *path, mode_t mode, int flag) {
    (void)dirfd; (void)path; (void)mode; (void)flag; return 0;
}
int utimes(const char *path, const struct timeval times[2]) {
    (void)path; (void)times; return 0;                                /* accept, ignore */
}
long pathconf(const char *path, int name) { (void)path; (void)name; errno = EINVAL; return -1; }
int symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath; errno = ENOSYS; return -1;
}
ssize_t readlink(const char *path, char *buf, size_t bufsize) {
    (void)path; (void)buf; (void)bufsize; errno = EINVAL; return -1;  /* not a symlink */
}
int link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath; errno = ENOSYS; return -1;
}

/* ---- scandir / alphasort -------------------------------------------------- */

/* Enumerate a directory into an allocated, optionally filtered and sorted array
   of dirent copies (readdir reuses one buffer, so each survivor is copied). The
   caller frees each entry and the array. */
int scandir(const char *dir, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    DIR *d = opendir(dir);
    struct dirent *ent, **list = NULL, **nl;
    size_t n = 0, cap = 0;
    if (!d)
        return -1;
    while ((ent = readdir(d)) != NULL) {
        struct dirent *copy;
        if (filter && !filter(ent))
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            nl = (struct dirent **)realloc(list, ncap * sizeof(*list));
            if (!nl)
                goto enomem;
            list = nl;
            cap = ncap;
        }
        copy = (struct dirent *)malloc(sizeof(struct dirent));
        if (!copy)
            goto enomem;
        *copy = *ent;
        list[n++] = copy;
    }
    closedir(d);
    if (compar && n > 1)
        qsort(list, n, sizeof(*list),
              (int (*)(const void *, const void *))compar);
    *namelist = list;
    return (int)n;

enomem:
    while (n)
        free(list[--n]);
    free(list);
    closedir(d);
    errno = ENOMEM;
    return -1;
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}
