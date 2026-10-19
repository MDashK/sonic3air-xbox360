/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/*
 * POSIX file I/O backend for picolibc on the Xbox 360 kernel, ported from the
 * original Xbox's libs/libc/xbox/fileio.c. This is the storage layer picolibc
 * stdio calls into (fopen/fread/fwrite/fseek/...) and what libc++'s <fstream>
 * sits on. It uses ONLY kernel exports (Nt/Rtl), never libxapi, so the
 * libc -> kernel layering stays acyclic.
 *
 * Paths are fully-qualified drive paths (game:\..., cache:\..., ...) with
 * backslash separators -- the 360 file system has no current-directory or
 * current-drive default (XDK "File Name Conventions"), so we do not synthesise
 * one; we only translate '/' to '\' for callers (libc++ <filesystem>) that join
 * with forward slashes. The kernel sees each path as an object name under
 * "\??\", where the drive links live (pathres.c). game:\ is the read-only title
 * media; cache:\ is the writable per-title utility drive, which crt_start.c
 * mounts with XMountUtilityDrive before main.
 *
 * fds 0/1/2 are the console: read(0)/write(1,2) route through the registered
 * libc hooks (libc_hooks.c), falling back to DbgPrint / EOF -- so this file
 * supersedes the read/write/lseek/close stubs libc_hooks.c used to provide.
 */

#define _GNU_SOURCE 1  /* picolibc gates open/O_*/struct stat behind POSIX visibility */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <unistd.h>

#include "libc_hooks.h"

extern int DbgPrint(const char *fmt, ...);

/* ---- NT kernel ABI (self-contained; matches the 360 xboxkrnl layout) ------ */

typedef long           NTSTATUS;
typedef void          *HANDLE;
typedef unsigned long  ULONG;
typedef unsigned short USHORT;

typedef struct { USHORT Length, MaximumLength; char *Buffer; } ANSI_STRING;
typedef struct { union { NTSTATUS Status; void *Pointer; }; unsigned long Information; } IO_STATUS_BLOCK;
/* The 360 OBJECT_ATTRIBUTES: RootDirectory, ObjectName, Attributes (no Length /
   SecurityDescriptor, unlike desktop NT). */
typedef struct { HANDLE RootDirectory; ANSI_STRING *ObjectName; ULONG Attributes; } OBJECT_ATTRIBUTES;
typedef union { long long QuadPart; } LARGE_INTEGER;
typedef struct {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER AllocationSize, EndOfFile;
    ULONG FileAttributes;
} FILE_NETWORK_OPEN_INFORMATION;
typedef struct { unsigned char DeleteFile; } FILE_DISPOSITION_INFORMATION;

extern void     RtlInitAnsiString(ANSI_STRING *dst, const char *src);
extern NTSTATUS NtCreateFile(HANDLE *handle, ULONG access, OBJECT_ATTRIBUTES *obja,
                             IO_STATUS_BLOCK *iosb, LARGE_INTEGER *alloc,
                             ULONG attrs, ULONG share, ULONG disp, ULONG options);
extern NTSTATUS NtReadFile(HANDLE h, HANDLE ev, void *apc, void *apcctx,
                           IO_STATUS_BLOCK *iosb, void *buf, ULONG len, LARGE_INTEGER *off);
extern NTSTATUS NtWriteFile(HANDLE h, HANDLE ev, void *apc, void *apcctx,
                            IO_STATUS_BLOCK *iosb, void *buf, ULONG len, LARGE_INTEGER *off);
extern NTSTATUS NtClose(HANDLE h);
extern NTSTATUS NtQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *info,
                                       ULONG len, int cls);
extern NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES *obja,
                                          FILE_NETWORK_OPEN_INFORMATION *info);
extern NTSTATUS NtSetInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *info,
                                     ULONG len, int cls);
extern NTSTATUS NtFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb);

#define NT_SUCCESS(s)  ((NTSTATUS)(s) >= 0)
#define STATUS_END_OF_FILE ((NTSTATUS)0xC0000011L)

#define NT_GENERIC_READ   0x80000000UL
#define NT_GENERIC_WRITE  0x40000000UL
#define NT_SYNCHRONIZE    0x00100000UL
#define NT_READ_ATTRS     0x00000080UL
#define NT_DELETE         0x00010000UL
#define NT_SHARE_RWD      0x00000007UL   /* READ|WRITE|DELETE */
#define NT_FILE_OPEN         1UL
#define NT_FILE_CREATE       2UL
#define NT_FILE_OPEN_IF      3UL
#define NT_FILE_OVERWRITE    4UL
#define NT_FILE_OVERWRITE_IF 5UL
#define NT_DIRECTORY_FILE     0x00000001UL
#define NT_SYNCHRONOUS_IO     0x00000020UL
#define NT_NON_DIRECTORY_FILE 0x00000040UL
#define NT_ATTR_NORMAL        0x00000080UL
#define NT_ATTR_DIRECTORY     0x00000010UL
#define OBJ_CASE_INSENSITIVE  0x00000040UL
#define FILE_NETWORK_OPEN_INFO 34
#define FILE_DISPOSITION_INFO  13

/* ---- fd table -------------------------------------------------------------- */

#define RXDK_FD_BASE 3
#define RXDK_FD_MAX  32

typedef struct {
    HANDLE    handle;
    long long offset;
    int       append;
    int       refcount;   /* dup'd fds share one open-file description */
} rxdk_ofd;

static rxdk_ofd *fd_table[RXDK_FD_MAX];   /* NULL = free */

static int alloc_slot(rxdk_ofd *o) {
    for (int i = RXDK_FD_BASE; i < RXDK_FD_MAX; ++i)
        if (!fd_table[i]) { fd_table[i] = o; return i; }
    return -1;
}
static rxdk_ofd *get_fd(int fd) {
    return (fd < RXDK_FD_BASE || fd >= RXDK_FD_MAX) ? NULL : fd_table[fd];
}

/* Shared with dirio.c (directory ops live there but reuse this fd table): map an
   fd to its kernel handle, and install a bare handle as a new file fd. */
void *__rxdk_fd_handle(int fd) {
    rxdk_ofd *o = get_fd(fd);
    return o ? o->handle : (void *)0;
}
int __rxdk_fd_install(void *h) {
    rxdk_ofd *o = (rxdk_ofd *)malloc(sizeof *o);
    int fd;
    if (!o) return -1;
    o->handle = (HANDLE)h;
    o->offset = 0;
    o->append = 0;
    o->refcount = 1;
    fd = alloc_slot(o);
    if (fd < 0) { free(o); return -1; }
    return fd;
}

/* Resolve a path to the kernel object name of a fully-qualified 360 path
   (translate '/'->'\', apply the libc cwd to relative paths, collapse ./..,
   prefix "\??\"). Shared with dirio.c; defined in pathres.c alongside
   getcwd/chdir. */
#define RXDK_PATH_BUF 1024
extern const char *__rxdk_nt_path(const char *in, char *out, size_t n);
extern const char *__rxdk_resolve_path(const char *in, char *out, size_t n);

static NTSTATUS nt_open(const char *path, ULONG access, ULONG disp,
                        ULONG options, HANDLE *out) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES obja;
    IO_STATUS_BLOCK iosb;
    char buf[RXDK_PATH_BUF];

/* S3AIR X360 PATCH (2026-10-17): open paths the way the XDK's CreateFile does -- the drive form
   ("game:\x") relative to the DOS-devices directory (RootDirectory = (HANDLE)-3) -- instead of
   "\??\game:\x" with no root. The console accepts both; Xenia only resolves the XDK form, so
   with upstream's form fopen/stat/opendir all fail under Xenia. See x360/README.md. */
    RtlInitAnsiString(&name, __rxdk_resolve_path(path, buf, sizeof buf));
    obja.RootDirectory = (HANDLE)(long)-3;
    obja.ObjectName = &name;
    obja.Attributes = OBJ_CASE_INSENSITIVE;
    return NtCreateFile(out, access | NT_SYNCHRONIZE | NT_READ_ATTRS, &obja,
                        &iosb, NULL, NT_ATTR_NORMAL, NT_SHARE_RWD, disp,
                        options | NT_SYNCHRONOUS_IO);
}

static long long file_size(HANDLE h) {
    /* The Xbox kernel rejects FileStandardInformation; FileNetworkOpenInformation
       carries EndOfFile and is supported. */
    FILE_NETWORK_OPEN_INFORMATION info;
    IO_STATUS_BLOCK iosb;
    if (!NT_SUCCESS(NtQueryInformationFile(h, &iosb, &info, sizeof info,
                                           FILE_NETWORK_OPEN_INFO)))
        return -1;
    return info.EndOfFile.QuadPart;
}

/* ---- open / read / write / lseek / close ---------------------------------- */

int open(const char *path, int flags, ...) {
    ULONG access, disp;
    HANDLE h;
    rxdk_ofd *o;
    int fd;

    switch (flags & O_ACCMODE) {
    case O_RDONLY: access = NT_GENERIC_READ; break;
    case O_WRONLY: access = NT_GENERIC_WRITE; break;
    default:       access = NT_GENERIC_READ | NT_GENERIC_WRITE; break;
    }
    if (flags & O_CREAT) {
        if (flags & O_EXCL)       disp = NT_FILE_CREATE;
        else if (flags & O_TRUNC) disp = NT_FILE_OVERWRITE_IF;
        else                      disp = NT_FILE_OPEN_IF;
    } else {
        disp = (flags & O_TRUNC) ? NT_FILE_OVERWRITE : NT_FILE_OPEN;
    }

    if (!NT_SUCCESS(nt_open(path, access, disp, NT_NON_DIRECTORY_FILE, &h))) {
        errno = ENOENT;
        return -1;
    }
    o = (rxdk_ofd *)malloc(sizeof *o);
    if (!o) { NtClose(h); errno = ENOMEM; return -1; }
    o->handle = h;
    o->offset = 0;
    o->append = (flags & O_APPEND) ? 1 : 0;
    o->refcount = 1;
    if (o->append) { long long sz = file_size(h); o->offset = sz > 0 ? sz : 0; }

    fd = alloc_slot(o);
    if (fd < 0) { NtClose(h); free(o); errno = EMFILE; return -1; }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
    rxdk_ofd *o;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    NTSTATUS st;

    if (fd == 0)
        return __rxdk_stdin_hook ? __rxdk_stdin_hook(buf, count) : 0;  /* EOF */

    o = get_fd(fd);
    if (!o) { errno = EBADF; return -1; }
    if (count == 0) return 0;

    off.QuadPart = o->offset;
    st = NtReadFile(o->handle, NULL, NULL, NULL, &iosb, buf, (ULONG)count, &off);
    if (st == STATUS_END_OF_FILE) return 0;
    if (!NT_SUCCESS(st)) { errno = EIO; return -1; }
    o->offset += (long long)iosb.Information;
    return (ssize_t)iosb.Information;
}

ssize_t write(int fd, const void *buf, size_t count) {
    rxdk_ofd *o;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    NTSTATUS st;

    if (!buf || count == 0) return 0;

    if (fd >= 0 && fd < RXDK_FD_BASE) {          /* console: hook, else DbgPrint */
        char line[512];
        size_t n = count;
        if (__rxdk_output_hook)
            return __rxdk_output_hook(fd, buf, count);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, buf, n);
        line[n] = '\0';
        DbgPrint("%s", line);
        return (ssize_t)count;
    }

    o = get_fd(fd);
    if (!o) { errno = EBADF; return -1; }
    if (o->append) { long long sz = file_size(o->handle); if (sz >= 0) o->offset = sz; }
    off.QuadPart = o->offset;
    st = NtWriteFile(o->handle, NULL, NULL, NULL, &iosb, (void *)(size_t)buf,
                     (ULONG)count, &off);
    if (!NT_SUCCESS(st)) { errno = EIO; return -1; }
    o->offset += (long long)iosb.Information;
    return (ssize_t)iosb.Information;
}

off_t lseek(int fd, off_t offset, int whence) {
    rxdk_ofd *o = get_fd(fd);
    long long base, pos;
    if (!o) { errno = EBADF; return -1; }
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = o->offset; break;
    case SEEK_END: base = file_size(o->handle); if (base < 0) base = 0; break;
    default: errno = EINVAL; return -1;
    }
    pos = base + (long long)offset;
    if (pos < 0) { errno = EINVAL; return -1; }
    o->offset = pos;
    return (off_t)pos;
}

int close(int fd) {
    rxdk_ofd *o;
    if (fd >= 0 && fd < RXDK_FD_BASE) return 0;   /* console */
    o = get_fd(fd);
    if (!o) { errno = EBADF; return -1; }
    fd_table[fd] = NULL;
    if (--o->refcount <= 0) {          /* last fd on this open-file description */
        if (o->handle) NtClose(o->handle);
        free(o);
    }
    return 0;
}

/* creat(path, mode) == open for writing, create/truncate. */
int creat(const char *path, mode_t mode) {
    return open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/* dup / dup2: a second fd onto the SAME open-file description (shared offset),
   the POSIX contract. Refcounted so close() frees the handle only once. */
int dup(int fd) {
    rxdk_ofd *o = get_fd(fd);
    int nfd;
    if (!o) { errno = EBADF; return -1; }
    nfd = alloc_slot(o);
    if (nfd < 0) { errno = EMFILE; return -1; }
    ++o->refcount;
    return nfd;
}

int dup2(int fd, int newfd) {
    rxdk_ofd *o = get_fd(fd);
    if (!o) { errno = EBADF; return -1; }
    if (newfd < RXDK_FD_BASE || newfd >= RXDK_FD_MAX) { errno = EBADF; return -1; }
    if (newfd == fd) return newfd;
    if (fd_table[newfd]) close(newfd);
    fd_table[newfd] = o;
    ++o->refcount;
    return newfd;
}

/* dup3: like dup2 but the two fds must differ and it carries O_CLOEXEC, which
   has no meaning without exec on the console -- so the flag is ignored. */
int dup3(int fd, int newfd, int flags) {
    (void)flags;
    if (fd == newfd) { errno = EINVAL; return -1; }
    return dup2(fd, newfd);
}

/* pread / pwrite: I/O at an explicit offset without disturbing the fd's own
   offset (the kernel Nt calls already take a byte offset). */
ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    rxdk_ofd *o = get_fd(fd);
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    NTSTATUS st;
    if (!o) { errno = EBADF; return -1; }
    if (count == 0) return 0;
    off.QuadPart = (long long)offset;
    st = NtReadFile(o->handle, NULL, NULL, NULL, &iosb, buf, (ULONG)count, &off);
    if (st == STATUS_END_OF_FILE) return 0;
    if (!NT_SUCCESS(st)) { errno = EIO; return -1; }
    return (ssize_t)iosb.Information;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    rxdk_ofd *o = get_fd(fd);
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    NTSTATUS st;
    if (!o) { errno = EBADF; return -1; }
    if (!buf || count == 0) return 0;
    off.QuadPart = (long long)offset;
    st = NtWriteFile(o->handle, NULL, NULL, NULL, &iosb, (void *)(size_t)buf,
                     (ULONG)count, &off);
    if (!NT_SUCCESS(st)) { errno = EIO; return -1; }
    return (ssize_t)iosb.Information;
}

/* fsync / fdatasync: flush the file's buffers to the device. */
int fsync(int fd) {
    rxdk_ofd *o = get_fd(fd);
    IO_STATUS_BLOCK iosb;
    if (fd >= 0 && fd < RXDK_FD_BASE) return 0;      /* console: nothing to flush */
    if (!o) { errno = EBADF; return -1; }
    NtFlushBuffersFile(o->handle, &iosb);
    return 0;
}
int fdatasync(int fd) { return fsync(fd); }

/* sync(): flush every open file description's buffers to the volume. */
void sync(void) {
    IO_STATUS_BLOCK iosb;
    for (int i = RXDK_FD_BASE; i < RXDK_FD_MAX; ++i)
        if (fd_table[i] && fd_table[i]->handle)
            NtFlushBuffersFile(fd_table[i]->handle, &iosb);
}

/* fcntl: F_DUPFD duplicates; the flag getters/setters are accepted as no-ops
   (the fd flags the 360 file API exposes are fixed). */
int fcntl(int fd, int cmd, ...) {
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        return dup(fd);
    case F_GETFD:
    case F_GETFL:
        if (!get_fd(fd)) { errno = EBADF; return -1; }
        return 0;
    case F_SETFD:
    case F_SETFL:
        if (!get_fd(fd)) { errno = EBADF; return -1; }
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

/* ---- stat / unlink / mkdir / rmdir ---------------------------------------- */

static void set_stat(struct stat *st, long long size, unsigned long attrs) {
    memset(st, 0, sizeof *st);
    st->st_size = (off_t)size;
    st->st_mode = (attrs & NT_ATTR_DIRECTORY) ? S_IFDIR : S_IFREG;
}

int fstat(int fd, struct stat *st) {
    rxdk_ofd *o = get_fd(fd);
    FILE_NETWORK_OPEN_INFORMATION info;
    IO_STATUS_BLOCK iosb;
    long long size = 0;
    unsigned long attrs = 0;
    if (!o || !st) { errno = EBADF; return -1; }
    if (NT_SUCCESS(NtQueryInformationFile(o->handle, &iosb, &info, sizeof info,
                                          FILE_NETWORK_OPEN_INFO))) {
        size = info.EndOfFile.QuadPart;
        attrs = info.FileAttributes;
    }
    set_stat(st, size, attrs);
    return 0;
}

int stat(const char *path, struct stat *st) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    FILE_NETWORK_OPEN_INFORMATION info;
    long long size = 0;
    unsigned long attrs = 0;
    if (!st) { errno = EINVAL; return -1; }
    /* Open (access=0 -> just SYNCHRONIZE|READ_ATTRIBUTES from nt_open, which
       opens files AND directories) and query the HANDLE. The path-based
       NtQueryFullAttributesFile reports EndOfFile from the cached directory
       entry, which is stale under xenia for a file just written through a
       handle; the handle query returns the live size. */
    if (!NT_SUCCESS(nt_open(path, 0, NT_FILE_OPEN, 0, &h))) { errno = ENOENT; return -1; }
    if (NT_SUCCESS(NtQueryInformationFile(h, &iosb, &info, sizeof info,
                                          FILE_NETWORK_OPEN_INFO))) {
        size = info.EndOfFile.QuadPart;
        attrs = info.FileAttributes;
    }
    NtClose(h);
    set_stat(st, size, attrs);
    return 0;
}

static int nt_delete(const char *path, ULONG options) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    FILE_DISPOSITION_INFORMATION dispose;
    if (!NT_SUCCESS(nt_open(path, NT_DELETE, NT_FILE_OPEN, options, &h))) {
        errno = ENOENT; return -1;
    }
    dispose.DeleteFile = 1;
    if (!NT_SUCCESS(NtSetInformationFile(h, &iosb, &dispose, sizeof dispose,
                                         FILE_DISPOSITION_INFO))) {
        NtClose(h); errno = EIO; return -1;
    }
    NtClose(h);
    return 0;
}

int unlink(const char *path) { return nt_delete(path, NT_NON_DIRECTORY_FILE); }
int rmdir(const char *path)  { return nt_delete(path, NT_DIRECTORY_FILE); }

int mkdir(const char *path, mode_t mode) {
    HANDLE h;
    (void)mode;
    if (!NT_SUCCESS(nt_open(path, NT_GENERIC_READ, NT_FILE_CREATE,
                            NT_DIRECTORY_FILE, &h))) {
        errno = EEXIST; return -1;
    }
    NtClose(h);
    return 0;
}

/* ---- bulk copy + advisory/preallocation --------------------------------- */

extern int ftruncate(int fd, off_t length);   /* dirio.c */

/* sendfile: copy `count` bytes from in_fd to out_fd through a bounce buffer
   (the console has no zero-copy path). With `offset` non-NULL the source is read
   at that position without disturbing in_fd's own offset, and *offset is
   advanced; otherwise in_fd's current offset is used. */
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
    char   buf[4096];
    size_t done = 0;
    off_t  pos = offset ? *offset : 0;
    while (done < count) {
        size_t chunk = count - done;
        ssize_t r, w;
        if (chunk > sizeof buf) chunk = sizeof buf;
        r = offset ? pread(in_fd, buf, chunk, pos) : read(in_fd, buf, chunk);
        if (r < 0) return -1;
        if (r == 0) break;
        w = write(out_fd, buf, (size_t)r);
        if (w < 0) return -1;
        done += (size_t)w;
        pos += w;
        if (w < r) break;
    }
    if (offset) *offset = pos;
    return (ssize_t)done;
}

/* copy_file_range: like sendfile but both ends may carry an explicit offset. */
ssize_t copy_file_range(int in_fd, off_t *in_off, int out_fd, off_t *out_off,
                        size_t len, unsigned int flags) {
    char   buf[4096];
    size_t done = 0;
    off_t  ip = in_off ? *in_off : 0, op = out_off ? *out_off : 0;
    (void)flags;
    while (done < len) {
        size_t chunk = len - done;
        ssize_t r, w;
        if (chunk > sizeof buf) chunk = sizeof buf;
        r = in_off ? pread(in_fd, buf, chunk, ip) : read(in_fd, buf, chunk);
        if (r < 0) return -1;
        if (r == 0) break;
        w = out_off ? pwrite(out_fd, buf, (size_t)r, op) : write(out_fd, buf, (size_t)r);
        if (w < 0) return -1;
        done += (size_t)w;
        ip += w;
        op += w;
        if (w < r) break;
    }
    if (in_off)  *in_off = ip;
    if (out_off) *out_off = op;
    return (ssize_t)done;
}

/* posix_fadvise: no cache hints on the console -> accept and ignore. */
int posix_fadvise(int fd, off_t offset, off_t len, int advice) {
    (void)fd; (void)offset; (void)len; (void)advice;
    return 0;
}

/* posix_fallocate: ensure the file is at least offset+len bytes (grow only).
   Returns an error number directly, not via errno. */
int posix_fallocate(int fd, off_t offset, off_t len) {
    struct stat st;
    off_t need;
    if (offset < 0 || len <= 0) return EINVAL;
    if (fstat(fd, &st) != 0) return errno;
    need = offset + len;
    if (st.st_size < need && ftruncate(fd, need) != 0) return errno;
    return 0;
}

/* Marker so fix_rxdk_ppc64.py can tell this patched object is already in libc.a */
const char __s3air_xdk_paths_patch[] = "s3air xdk-paths 1";
