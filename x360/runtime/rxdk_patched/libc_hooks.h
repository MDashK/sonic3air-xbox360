/*
 * libc I/O + process hooks (RXDK extension), mirrored from RXDK-Libs.
 *
 * libc stays kernel-only, so it cannot own platform policy like where console
 * input comes from or how an image is launched. It exposes these registration
 * points instead: an app (or libxapi) installs a handler and libc routes the
 * relevant call through it. With none set, libc falls back to a safe default
 * (EOF for stdin, DbgPrint for output, -1/ENOSYS for exec).
 */
#ifndef RXDK_LIBC_HOOKS_H
#define RXDK_LIBC_HOOKS_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* stdin: read(0, buf, count) calls this. Return bytes read, 0 = EOF, -1 = error. */
typedef ssize_t (*rxdk_stdin_fn)(void *buf, size_t count);
void rxdk_set_stdin_handler(rxdk_stdin_fn fn);

/* output: write(1/2, buf, count) calls this instead of DbgPrint. Return bytes written. */
typedef ssize_t (*rxdk_output_fn)(int fd, const void *buf, size_t count);
void rxdk_set_output_handler(rxdk_output_fn fn);

/* exec: execve(path, argv, envp) calls this. Returns like execve (no return on success). */
typedef int (*rxdk_exec_fn)(const char *path, char *const argv[], char *const envp[]);
void rxdk_set_exec_handler(rxdk_exec_fn fn);

/* The installed handlers (NULL when unset). libc reads these directly. */
extern rxdk_stdin_fn __rxdk_stdin_hook;
extern rxdk_output_fn __rxdk_output_hook;
extern rxdk_exec_fn __rxdk_exec_hook;

#ifdef __cplusplus
}
#endif

#endif /* RXDK_LIBC_HOOKS_H */
