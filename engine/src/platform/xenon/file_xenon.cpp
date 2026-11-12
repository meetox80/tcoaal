#include <dirent.h>
#include <ppc/register.h>
#include <ppc/xenonsprs.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "io_xenon.h"

namespace xenon_io {

constexpr size_t READ_BUFFER = 32 * 1024;

alignas(128) static volatile unsigned int g_ticket = 0;
alignas(128) static volatile unsigned int g_serving = 0;
static volatile int g_owner = -1;
static int g_depth = 0;

int thread_id() { return (int)mfspr(pir); }

void lock() {
    int id = thread_id();
    if (g_owner == id) {
        g_depth++;
        return;
    }
    unsigned int t = __atomic_fetch_add(&g_ticket, 1u, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_serving, __ATOMIC_ACQUIRE) != t) {
        asm volatile("or 1,1,1");
        while (__atomic_load_n(&g_serving, __ATOMIC_ACQUIRE) != t) {
        }
        asm volatile("or 2,2,2");
    }
    g_owner = id;
    g_depth = 1;
}

void unlock() {
    if (--g_depth > 0) return;
    g_owner = -1;
    __atomic_store_n(&g_serving, g_serving + 1, __ATOMIC_RELEASE);
}

}

extern "C" {

int __real_open(const char* path, int flags, int mode);
ssize_t __real_read(int fd, void* dst, size_t len);
ssize_t __real_write(int fd, const void* src, size_t len);
off_t __real_lseek(int fd, off_t pos, int whence);
int __real_close(int fd);
int __real_fstat(int fd, struct stat* st);
int __real_stat(const char* path, struct stat* st);
int __real_mkdir(const char* path, mode_t mode);
int __real_unlink(const char* path);
int __real_rename(const char* from, const char* to);
int __real_ftruncate(int fd, off_t len);
int __real_fsync(int fd);
DIR* __real_opendir(const char* path);
struct dirent* __real_readdir(DIR* dir);
int __real_closedir(DIR* dir);
FILE* __real_fopen(const char* path, const char* mode);
int __real_fclose(FILE* f);

int __wrap_open(const char* path, int flags, int mode) {
    xenon_io::Guard g;
    return __real_open(path, flags, mode);
}

ssize_t __wrap_read(int fd, void* dst, size_t len) {
    xenon_io::Guard g;
    return __real_read(fd, dst, len);
}

ssize_t __wrap_write(int fd, const void* src, size_t len) {
    xenon_io::Guard g;
    return __real_write(fd, src, len);
}

off_t __wrap_lseek(int fd, off_t pos, int whence) {
    xenon_io::Guard g;
    return __real_lseek(fd, pos, whence);
}

int __wrap_close(int fd) {
    xenon_io::Guard g;
    return __real_close(fd);
}

int __wrap_fstat(int fd, struct stat* st) {
    xenon_io::Guard g;
    return __real_fstat(fd, st);
}

int __wrap_stat(const char* path, struct stat* st) {
    xenon_io::Guard g;
    return __real_stat(path, st);
}

int __wrap_mkdir(const char* path, mode_t mode) {
    xenon_io::Guard g;
    return __real_mkdir(path, mode);
}

int __wrap_unlink(const char* path) {
    xenon_io::Guard g;
    return __real_unlink(path);
}

int __wrap_rename(const char* from, const char* to) {
    xenon_io::Guard g;
    return __real_rename(from, to);
}

int __wrap_ftruncate(int fd, off_t len) {
    xenon_io::Guard g;
    return __real_ftruncate(fd, len);
}

int __wrap_fsync(int fd) {
    xenon_io::Guard g;
    return __real_fsync(fd);
}

DIR* __wrap_opendir(const char* path) {
    xenon_io::Guard g;
    return __real_opendir(path);
}

struct dirent* __wrap_readdir(DIR* dir) {
    xenon_io::Guard g;
    return __real_readdir(dir);
}

int __wrap_closedir(DIR* dir) {
    xenon_io::Guard g;
    return __real_closedir(dir);
}

FILE* __wrap_fopen(const char* path, const char* mode) {
    xenon_io::Guard g;
    FILE* f = __real_fopen(path, mode);
    if (f && mode[0] == 'r' && !std::strchr(mode, '+')) std::setvbuf(f, nullptr, _IOFBF, xenon_io::READ_BUFFER);
    return f;
}

int __wrap_fclose(FILE* f) {
    xenon_io::Guard g;
    return __real_fclose(f);
}

}
