#include "Hooks.h"
#include "RedirectTable.h"
#include "HookRegistry.h"
#include <android/log.h>
#include <fcntl.h>
#include <stdarg.h>
#include <cstring>
#include <link.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <mutex>
#include <string>
#include <errno.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <vector>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef __NR_memfd_create
#define __NR_memfd_create 279
#endif

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" bool InstallInlineHook(void*, void*, void**);
extern "C" void** GetGotAddress(const char*, const char*);
extern "C" HookResult HookGotEntryEx(void**, void*, void**, const char*, const char*, HookHandle**);
extern "C" HookResult UnhookGotEntry(void*);
extern "C" HookResult UninstallInlineHook(void*);

int32_t g_virtualUid = 10000;
int32_t g_virtualGid = 10000;
void SetVirtualUid(int32_t uid) { g_virtualUid = uid; g_virtualGid = uid; }
std::atomic<int> g_systemCallDepth{0};

int (*_Original_open)(const char*, int, ...) = nullptr;
int (*_Original_open64)(const char*, int, ...) = nullptr;
int (*_Original__open_2)(const char*, int) = nullptr;
int (*_Original_openat)(int, const char*, int, ...) = nullptr;
int (*_Original_openat64)(int, const char*, int, ...) = nullptr;
int (*_Original__openat_2)(int, const char*, int) = nullptr;
int (*_Original_stat)(const char*, struct stat*) = nullptr;
int (*_Original_lstat)(const char*, struct stat*) = nullptr;
int (*_Original_stat64)(const char*, struct stat64*) = nullptr;
int (*_Original_lstat64)(const char*, struct stat64*) = nullptr;
int (*_Original___stat)(const char*, struct stat*) = nullptr;
int (*_Original___lstat)(const char*, struct stat*) = nullptr;
int (*_Original___stat64)(const char*, struct stat64*) = nullptr;
int (*_Original___fstatat64)(int, const char*, struct stat*, int) = nullptr;
int (*_Original_statx)(int, const char*, int, unsigned int, struct statx*) = nullptr;
int (*_Original_fstatat)(int, const char*, struct stat*, int) = nullptr;
int (*_Original_access)(const char*, int) = nullptr;
int (*_Original_faccessat)(int, const char*, int, int) = nullptr;
ssize_t (*_Original_readlink)(const char*, char*, size_t) = nullptr;
int (*_Original_unlink)(const char*) = nullptr;
int (*_Original_unlinkat)(int, const char*, int) = nullptr;
int (*_Original_rename)(const char*, const char*) = nullptr;
int (*_Original_renameat)(int, const char*, int, const char*) = nullptr;
int (*_Original_mkdir)(const char*, mode_t) = nullptr;
int (*_Original_mkdirat)(int, const char*, mode_t) = nullptr;
int (*_Original_rmdir)(const char*) = nullptr;
FILE* (*_Original_fopen)(const char*, const char*) = nullptr;
FILE* (*_Original_fopen64)(const char*, const char*) = nullptr;
DIR*  (*_Original_opendir)(const char*) = nullptr;
struct dirent*   (*_Original_readdir)(DIR*) = nullptr;
struct dirent64* (*_Original_readdir64)(DIR*) = nullptr;
int (*_Original_getdents64)(unsigned int, struct dirent64*, unsigned int) = nullptr;
void* (*_Original_dlopen)(const char*, int) = nullptr;
void* (*_Original_android_dlopen_ext)(const char*, int, const void*) = nullptr;
int   (*_Original___system_property_get)(const char*, char*) = nullptr;
const void* (*_Original___system_property_find)(const char*) = nullptr;
void  (*_Original___system_property_read_callback)(
        const void*, void(*)(void*, const char*, const char*, uint32_t), void*) = nullptr;
int   (*_Original___system_property_read)(const void*, char*, char*) = nullptr;
uid_t (*_Original_getuid)() = nullptr;
uid_t (*_Original_geteuid)() = nullptr;
gid_t (*_Original_getgid)() = nullptr;
gid_t (*_Original_getegid)() = nullptr;

static bool ShouldHidePath(const char* path) {
    if (!path) return false;
    if (strncmp(path, "/data/adb/", 10) == 0) return true;
    if (strncmp(path, "/sbin/.magisk", 13) == 0) return true;
    static const char* kHidden[] = {
            "/system/bin/su", "/system/xbin/su", "/sbin/su",
            "/system/sd/xbin/su", "/system/bin/failsafe/su",
            "/system/xbin/daemonsu", "/system/app/Superuser.apk",
            "/system/app/SuperSU/SuperSU.apk", "/system/etc/.has_su_daemon",
            "/system/bin/.ext/.su", "/system/usr/we-need-root/su-backup",
            nullptr
    };
    for (int i = 0; kHidden[i]; i++)
        if (strcmp(path, kHidden[i]) == 0) return true;
    return false;
}

static bool ShouldHideDirent(const char* name) {
    if (!name) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (strstr(name, "dualhost"))      return true;
    if (strstr(name, "libdualcore"))   return true;
    if (strstr(name, "libhookengine")) return true;
    if (strstr(name, "ims134"))        return true;
    return false;
}

struct PropSpoof { const char* name; const char* value; };
static const PropSpoof kPropSpoofs[] = {
        {"ro.debuggable","0"}, {"ro.secure","1"}, {"ro.build.tags","release-keys"},
        {"ro.build.type","user"}, {"ro.kernel.qemu",""}, {"ro.boot.qemu",""},
        {"init.svc.adbd","stopped"}, {"service.adb.root",""}, {"sys.oem_unlock_allowed","0"},
        {nullptr, nullptr}
};

static const char* LookupSpoof(const char* name) {
    if (!name) return nullptr;
    for (int i = 0; kPropSpoofs[i].name; i++)
        if (strcmp(name, kPropSpoofs[i].name) == 0) return kPropSpoofs[i].value;
    return nullptr;
}

#define PROP_VALUE_MAX 92
struct FakeProp { uint32_t serial; char value[PROP_VALUE_MAX]; char name[64]; int in_use; };
static FakeProp g_fakeProps[16];
static std::mutex g_fakePropMutex;

static FakeProp* AllocFakeProp(const char* name, const char* value) {
    std::lock_guard<std::mutex> lock(g_fakePropMutex);
    for (int i = 0; i < 16; i++)
        if (g_fakeProps[i].in_use && strcmp(g_fakeProps[i].name, name) == 0) return &g_fakeProps[i];
    for (int i = 0; i < 16; i++) {
        if (!g_fakeProps[i].in_use) {
            g_fakeProps[i].in_use = 1;
            g_fakeProps[i].serial = 1;
            memset(g_fakeProps[i].value, 0, PROP_VALUE_MAX);
            strncpy(g_fakeProps[i].value, value, PROP_VALUE_MAX - 1);
            memset(g_fakeProps[i].name, 0, sizeof(g_fakeProps[i].name));
            strncpy(g_fakeProps[i].name, name, sizeof(g_fakeProps[i].name) - 1);
            return &g_fakeProps[i];
        }
    }
    return nullptr;
}

static bool IsFakeProp(const void* pi) {
    uintptr_t p = (uintptr_t)pi;
    uintptr_t s = (uintptr_t)&g_fakeProps[0];
    return p >= s && p < s + sizeof(g_fakeProps);
}

static int Fake___system_property_get(const char* name, char* value) {
    const char* spoof = LookupSpoof(name);
    if (spoof) {
        if (value) { size_t n = strlen(spoof); memcpy(value, spoof, n); value[n] = 0; }
        return (int)strlen(spoof);
    }
    if (!_Original___system_property_get) return 0;
    return _Original___system_property_get(name, value);
}

static const void* Fake___system_property_find(const char* name) {
    const char* spoof = LookupSpoof(name);
    if (spoof) { FakeProp* fp = AllocFakeProp(name, spoof); if (fp) return fp; }
    if (!_Original___system_property_find) return nullptr;
    return _Original___system_property_find(name);
}

static void Fake___system_property_read_callback(
        const void* pi, void (*cb)(void*, const char*, const char*, uint32_t), void* cookie) {
    if (IsFakeProp(pi)) {
        FakeProp* fp = (FakeProp*)pi;
        cb(cookie, fp->name, fp->value, fp->serial);
        return;
    }
    if (_Original___system_property_read_callback)
        _Original___system_property_read_callback(pi, cb, cookie);
}

static int Fake___system_property_read(const void* pi, char* name, char* value) {
    if (IsFakeProp(pi)) {
        FakeProp* fp = (FakeProp*)pi;
        if (name)  { size_t n = strlen(fp->name);  memcpy(name,  fp->name,  n + 1); }
        if (value) { size_t n = strlen(fp->value); memcpy(value, fp->value, n + 1); }
        return (int)strlen(fp->value);
    }
    if (!_Original___system_property_read) return 0;
    return _Original___system_property_read(pi, name, value);
}

static uid_t Fake_getuid() {
    if (g_systemCallDepth.load(std::memory_order_acquire) > 0)
        return (uid_t)syscall(SYS_getuid);
    return (uid_t)g_virtualUid;
}

static uid_t Fake_geteuid() {
    if (g_systemCallDepth.load(std::memory_order_acquire) > 0)
        return (uid_t)syscall(SYS_geteuid);
    return (uid_t)g_virtualUid;
}

static gid_t Fake_getgid() {
    if (g_systemCallDepth.load(std::memory_order_acquire) > 0)
        return (gid_t)syscall(SYS_getgid);
    return (gid_t)g_virtualGid;
}

static gid_t Fake_getegid() {
    if (g_systemCallDepth.load(std::memory_order_acquire) > 0)
        return (gid_t)syscall(SYS_getegid);
    return (gid_t)g_virtualGid;
}

static std::mutex g_mapsMutex;
static std::string g_filteredMaps;
static int64_t g_lastMapsGen = 0;

static int64_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static std::string BuildFilteredMaps() {
    int fd = (int)syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    std::string raw;
    char buf[8192];
    ssize_t n;
    while ((n = (ssize_t)syscall(__NR_read, fd, buf, sizeof(buf))) > 0)
        raw.append(buf, n);
    syscall(__NR_close, fd);
    std::string out;
    out.reserve(raw.size());
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        std::string line(raw.c_str() + pos, eol - pos);
        bool hide =
                line.find("com.ims134.dualhost")   != std::string::npos ||
                line.find("com.ims134.hookengine") != std::string::npos ||
                line.find("libdualcore.so")        != std::string::npos ||
                line.find("libhookengine.so")      != std::string::npos;
        if (!hide) { out.append(raw, pos, eol - pos); out.push_back('\n'); }
        pos = eol + 1;
    }
    return out;
}

static std::string GetFilteredMaps() {
    std::lock_guard<std::mutex> lock(g_mapsMutex);
    int64_t now = NowMs();
    if (!g_filteredMaps.empty() && (now - g_lastMapsGen) < 2000) return g_filteredMaps;
    g_filteredMaps = BuildFilteredMaps();
    g_lastMapsGen = now;
    return g_filteredMaps;
}

static int OpenFilteredMapsFd() {
    std::string m = GetFilteredMaps();
    if (m.empty()) return -1;
    int fd = (int)syscall(__NR_memfd_create, "dhmaps", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (syscall(__NR_write, fd, m.data(), m.size()) != (ssize_t)m.size()) {
        syscall(__NR_close, fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static bool IsMapsPath(const char* path) {
    if (!path) return false;
    if (strcmp(path, "/proc/self/maps") == 0) return true;
    static char pidPath[64] = {0};
    static pid_t cachedPid = -1;
    pid_t pid = getpid();
    if (pid != cachedPid) {
        snprintf(pidPath, sizeof(pidPath), "/proc/%d/maps", pid);
        cachedPid = pid;
    }
    return strcmp(path, pidPath) == 0;
}

int Fake_open(const char* path, int flags, ...) {
    if (ShouldHidePath(path)) { errno = ENOENT; return -1; }
    mode_t mode = 0;
    bool hasMode = (flags & (O_CREAT | O_TMPFILE)) != 0;
    if (hasMode) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    if (IsMapsPath(path) && (flags & O_ACCMODE) == O_RDONLY) {
        int fd = OpenFilteredMapsFd();
        if (fd >= 0) return fd;
    }
    if (!_Original_open) { errno = ENOSYS; return -1; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    const char* p = np.empty() ? path : np.c_str();
    return hasMode ? _Original_open(p, flags, mode) : _Original_open(p, flags);
}

int Fake_open64(const char* path, int flags, ...) {
    if (ShouldHidePath(path)) { errno = ENOENT; return -1; }
    mode_t mode = 0;
    bool hasMode = (flags & (O_CREAT | O_TMPFILE)) != 0;
    if (hasMode) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    if (IsMapsPath(path) && (flags & O_ACCMODE) == O_RDONLY) {
        int fd = OpenFilteredMapsFd();
        if (fd >= 0) return fd;
    }
    if (!_Original_open64) { errno = ENOSYS; return -1; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    const char* p = np.empty() ? path : np.c_str();
    return hasMode ? _Original_open64(p, flags, mode) : _Original_open64(p, flags);
}

int Fake__open_2(const char* path, int flags) {
    if (ShouldHidePath(path)) { errno = ENOENT; return -1; }
    if (IsMapsPath(path) && (flags & O_ACCMODE) == O_RDONLY) {
        int fd = OpenFilteredMapsFd();
        if (fd >= 0) return fd;
    }
    if (!_Original__open_2) { errno = ENOSYS; return -1; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    return _Original__open_2(np.empty() ? path : np.c_str(), flags);
}

int Fake_openat(int dfd, const char* path, int flags, ...) {
    if (ShouldHidePath(path)) { errno = ENOENT; return -1; }
    mode_t mode = 0;
    bool hasMode = (flags & (O_CREAT | O_TMPFILE)) != 0;
    if (hasMode) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    if (IsMapsPath(path) && (flags & O_ACCMODE) == O_RDONLY) {
        int fd = OpenFilteredMapsFd();
        if (fd >= 0) return fd;
    }
    if (!_Original_openat) { errno = ENOSYS; return -1; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    const char* p = np.empty() ? path : np.c_str();
    return hasMode ? _Original_openat(dfd, p, flags, mode) : _Original_openat(dfd, p, flags);
}

int Fake_openat64(int dfd, const char* path, int flags, ...) {
    if (ShouldHidePath(path)) { errno = ENOENT; return -1; }
    mode_t mode = 0;
    bool hasMode = (flags & (O_CREAT | O_TMPFILE)) != 0;
    if (hasMode) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    if (IsMapsPath(path) && (flags & O_ACCMODE) == O_RDONLY) {
        int fd = OpenFilteredMapsFd();
        if (fd >= 0) return fd;
    }
    if (!_Original_openat64) { errno = ENOSYS; return -1; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    const char* p = np.empty() ? path : np.c_str();
    return hasMode ? _Original_openat64(dfd, p, flags, mode) : _Original_openat64(dfd, p, flags);
}

int Fake__openat_2(int dfd, const char* path, int flags) {
    if (ShouldHidePath(path)) { errno = ENOENT; return -1; }
    if (IsMapsPath(path) && (flags & O_ACCMODE) == O_RDONLY) {
        int fd = OpenFilteredMapsFd();
        if (fd >= 0) return fd;
    }
    if (!_Original__openat_2) { errno = ENOSYS; return -1; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    return _Original__openat_2(dfd, np.empty() ? path : np.c_str(), flags);
}

FILE* Fake_fopen(const char* path, const char* mode) {
    if (ShouldHidePath(path)) { errno = ENOENT; return nullptr; }
    if (path && mode && IsMapsPath(path) && mode[0] == 'r' && !strchr(mode, '+')) {
        int fd = OpenFilteredMapsFd();
        if (fd >= 0) { FILE* f = fdopen(fd, mode); if (f) return f; close(fd); }
    }
    if (!_Original_fopen) { errno = ENOSYS; return nullptr; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    return _Original_fopen(np.empty() ? path : np.c_str(), mode);
}

FILE* Fake_fopen64(const char* path, const char* mode) {
    if (ShouldHidePath(path)) { errno = ENOENT; return nullptr; }
    if (path && mode && IsMapsPath(path) && mode[0] == 'r' && !strchr(mode, '+')) {
        int fd = OpenFilteredMapsFd();
        if (fd >= 0) { FILE* f = fdopen(fd, mode); if (f) return f; close(fd); }
    }
    if (!_Original_fopen64) { errno = ENOSYS; return nullptr; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    return _Original_fopen64(np.empty() ? path : np.c_str(), mode);
}

int Fake_stat(const char* p, struct stat* b) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_stat) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_stat(n.empty() ? p : n.c_str(), b);
}

int Fake_lstat(const char* p, struct stat* b) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_lstat) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_lstat(n.empty() ? p : n.c_str(), b);
}

int Fake_stat64(const char* p, struct stat64* b) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_stat64) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_stat64(n.empty() ? p : n.c_str(), b);
}

int Fake_lstat64(const char* p, struct stat64* b) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_lstat64) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_lstat64(n.empty() ? p : n.c_str(), b);
}

int Fake___stat(const char* p, struct stat* b) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original___stat) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original___stat(n.empty() ? p : n.c_str(), b);
}

int Fake___lstat(const char* p, struct stat* b) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original___lstat) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original___lstat(n.empty() ? p : n.c_str(), b);
}

int Fake___stat64(const char* p, struct stat64* b) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original___stat64) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original___stat64(n.empty() ? p : n.c_str(), b);
}

int Fake___fstatat64(int d, const char* p, struct stat* b, int f) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original___fstatat64) { errno = ENOSYS; return -1; }
    if (p && p[0]) {
        std::string n = RedirectTable::GetInstance().ResolvePath(p);
        if (!n.empty()) return _Original___fstatat64(d, n.c_str(), b, f);
    }
    return _Original___fstatat64(d, p, b, f);
}

int Fake_statx(int d, const char* p, int flags, unsigned int mask, struct statx* b) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_statx) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_statx(d, n.empty() ? p : n.c_str(), flags, mask, b);
}

int Fake_fstatat(int d, const char* p, struct stat* b, int f) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_fstatat) { errno = ENOSYS; return -1; }
    if (p && p[0]) {
        std::string n = RedirectTable::GetInstance().ResolvePath(p);
        if (!n.empty()) return _Original_fstatat(d, n.c_str(), b, f);
    }
    return _Original_fstatat(d, p, b, f);
}

int Fake_access(const char* p, int m) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_access) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_access(n.empty() ? p : n.c_str(), m);
}

int Fake_faccessat(int d, const char* p, int m, int f) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_faccessat) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_faccessat(d, n.empty() ? p : n.c_str(), m, f);
}

ssize_t Fake_readlink(const char* p, char* b, size_t s) {
    if (ShouldHidePath(p)) { errno = ENOENT; return -1; }
    if (!_Original_readlink) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_readlink(n.empty() ? p : n.c_str(), b, s);
}

int Fake_unlink(const char* p) {
    if (!_Original_unlink) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_unlink(n.empty() ? p : n.c_str());
}

int Fake_unlinkat(int d, const char* p, int f) {
    if (!_Original_unlinkat) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_unlinkat(d, n.empty() ? p : n.c_str(), f);
}

int Fake_rename(const char* o, const char* n) {
    if (!_Original_rename) { errno = ENOSYS; return -1; }
    std::string a = RedirectTable::GetInstance().ResolvePath(o);
    std::string b = RedirectTable::GetInstance().ResolvePath(n);
    return _Original_rename(a.empty() ? o : a.c_str(), b.empty() ? n : b.c_str());
}

int Fake_renameat(int od, const char* o, int nd, const char* n) {
    if (!_Original_renameat) { errno = ENOSYS; return -1; }
    std::string a = RedirectTable::GetInstance().ResolvePath(o);
    std::string b = RedirectTable::GetInstance().ResolvePath(n);
    return _Original_renameat(od, a.empty() ? o : a.c_str(), nd, b.empty() ? n : b.c_str());
}

int Fake_mkdir(const char* p, mode_t m) {
    if (!_Original_mkdir) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_mkdir(n.empty() ? p : n.c_str(), m);
}

int Fake_mkdirat(int d, const char* p, mode_t m) {
    if (!_Original_mkdirat) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_mkdirat(d, n.empty() ? p : n.c_str(), m);
}

int Fake_rmdir(const char* p) {
    if (!_Original_rmdir) { errno = ENOSYS; return -1; }
    std::string n = RedirectTable::GetInstance().ResolvePath(p);
    return _Original_rmdir(n.empty() ? p : n.c_str());
}

DIR* Fake_opendir(const char* path) {
    if (!_Original_opendir) { errno = ENOSYS; return nullptr; }
    std::string np = RedirectTable::GetInstance().ResolvePath(path);
    return _Original_opendir(np.empty() ? path : np.c_str());
}

struct dirent64* Fake_readdir64(DIR* d) {
    if (!_Original_readdir64) return nullptr;
    while (true) {
        struct dirent64* e = _Original_readdir64(d);
        if (!e) return nullptr;
        if (!ShouldHideDirent(e->d_name)) return e;
    }
}

struct dirent* Fake_readdir(DIR* d) {
    if (!_Original_readdir) return nullptr;
    while (true) {
        struct dirent* e = _Original_readdir(d);
        if (!e) return nullptr;
        if (!ShouldHideDirent(e->d_name)) return e;
    }
}

int Fake_getdents64(unsigned int fd, struct dirent64* dirp, unsigned int count) {
    if (!_Original_getdents64) return -1;
    int n = _Original_getdents64(fd, dirp, count);
    if (n <= 0) return n;
    char* dst = (char*)dirp;
    char* end = (char*)dirp + n;
    struct dirent64* d = dirp;
    int newCount = 0;
    while ((char*)d < end) {
        size_t reclen = d->d_reclen;
        if (reclen == 0 || (char*)d + reclen > end) break;
        if (!ShouldHideDirent(d->d_name)) {
            if (dst != (char*)d) memmove(dst, d, reclen);
            dst += reclen;
            newCount += reclen;
        }
        d = (struct dirent64*)((char*)d + reclen);
    }
    return newCount;
}

static std::mutex g_rescanMutex;
static std::unordered_set<std::string> g_seenLibs;
static std::atomic<bool> g_rescanPending{false};

// Forward declaration — definition is below.
static int HookLibraryCallback(struct dl_phdr_info* info, size_t, void* data);

static void ScheduleRescan() {
    if (g_rescanPending.exchange(true)) return;
    std::thread([]{
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_rescanPending = false;
        std::lock_guard<std::mutex> lock(g_rescanMutex);
        dl_iterate_phdr([](struct dl_phdr_info* info, size_t sz, void* d) -> int {
            if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
            if (g_seenLibs.find(info->dlpi_name) != g_seenLibs.end()) return 0;
            g_seenLibs.insert(info->dlpi_name);
            return HookLibraryCallback(info, sz, d);
        }, nullptr);
    }).detach();
}

void* Fake_dlopen(const char* name, int flags) {
    if (!_Original_dlopen)
        _Original_dlopen = (decltype(_Original_dlopen))dlsym(RTLD_DEFAULT, "dlopen");
    if (!_Original_dlopen) return nullptr;
    void* h = _Original_dlopen(name, flags);
    if (h && name) ScheduleRescan();
    return h;
}

void* Fake_android_dlopen_ext(const char* name, int flags, const void* extinfo) {
    if (!_Original_android_dlopen_ext)
        _Original_android_dlopen_ext = (decltype(_Original_android_dlopen_ext))dlsym(RTLD_DEFAULT, "android_dlopen_ext");
    if (!_Original_android_dlopen_ext) return Fake_dlopen(name, flags);
    void* h = _Original_android_dlopen_ext(name, flags, extinfo);
    if (h && name) ScheduleRescan();
    return h;
}

static bool ShouldHookProperty(const char* name) {
    if (!name || !name[0]) return false;
    if (strstr(name, "startup_agents")) return false;
    if (strstr(name, "libjavacore.so"))   return true;
    if (strstr(name, "libopenjdk.so"))    return true;
    if (strstr(name, "libopenjdkjvm.so")) return true;
    if (strstr(name, "/data/app/"))  return true;
    if (strstr(name, "/data/user/")) return true;
    if (strstr(name, "/data/data/")) return true;
    return false;
}

static bool ShouldHookDlopen(const char* name) {
    if (!name || !name[0]) return false;
    if (strstr(name, "libart.so"))          return true;
    if (strstr(name, "libnativeloader"))    return true;
    if (strstr(name, "libnativebridge"))    return true;
    if (strstr(name, "/data/app/"))  return true;
    if (strstr(name, "/data/user/")) return true;
    if (strstr(name, "/data/data/")) return true;
    return false;
}

extern "C" bool ShouldHookDlopenForTest(const char* name) {
    return ShouldHookDlopen(name);
}

static bool ShouldHookFileIO(const char* name) {
    if (!name || !name[0]) return false;
    if (strstr(name, "startup_agents")) return false;
    if (strstr(name, "libart.so"))        return true;
    if (strstr(name, "libjavacore.so"))   return true;
    if (strstr(name, "libopenjdk.so"))    return true;
    if (strstr(name, "libopenjdkjvm.so")) return true;
    if (strstr(name, "/data/app/"))  return true;
    if (strstr(name, "/data/user/")) return true;
    if (strstr(name, "/data/data/")) return true;
    return false;
}

struct InstallCtx {
    std::vector<HookHandle*> installed;
    bool failed = false;
    HookResult error = HOOK_OK;
    const char* fail_where = nullptr;
};

#define PATCH(sym, fake) do { \
    void** ga = GetGotAddress(info->dlpi_name, #sym); \
    if (!ga) { LOGD("  skip %s in %s (symbol absent)", #sym, info->dlpi_name); } \
    else { \
        HookHandle* h = nullptr; \
        HookResult r = HookGotEntryEx(ga, (void*)fake, (void**)&_Original_##sym, \
                                      info->dlpi_name, #sym, &h); \
        if (r == HOOK_OK) { ctx->installed.push_back(h); \
            LOGD("  hooked %s in %s", #sym, info->dlpi_name); } \
        else if (r == HOOK_ALREADY_HOOKED) { LOGD("  %s already hooked in %s", #sym, info->dlpi_name); } \
        else { LOGE("  FAILED %s in %s (r=%d)", #sym, info->dlpi_name, (int)r); \
               ctx->error = r; ctx->failed = true; ctx->fail_where = #sym; return 1; } \
    } \
} while (0)

#define PATCH_ALIAS(sym, fake, orig) do { \
    void** ga = GetGotAddress(info->dlpi_name, sym); \
    if (!ga) { LOGD("  skip %s in %s (symbol absent)", sym, info->dlpi_name); } \
    else { \
        HookHandle* h = nullptr; \
        HookResult r = HookGotEntryEx(ga, (void*)fake, (void**)&orig, \
                                      info->dlpi_name, sym, &h); \
        if (r == HOOK_OK) { ctx->installed.push_back(h); \
            LOGD("  hooked %s in %s", sym, info->dlpi_name); } \
        else if (r == HOOK_ALREADY_HOOKED) { LOGD("  %s already hooked in %s", sym, info->dlpi_name); } \
        else { LOGE("  FAILED %s in %s (r=%d)", sym, info->dlpi_name, (int)r); \
               ctx->error = r; ctx->failed = true; ctx->fail_where = sym; return 1; } \
    } \
} while (0)

static int HookLibraryCallback(struct dl_phdr_info* info, size_t, void* data) {
    InstallCtx* ctx = (InstallCtx*)data;
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
    if (strstr(info->dlpi_name, "libhookengine.so") ||
        strstr(info->dlpi_name, "libdualcore.so")) return 0;
    if (strstr(info->dlpi_name, "/libc.so")) return 0;

    if (ShouldHookProperty(info->dlpi_name)) {
        PATCH_ALIAS("__system_property_get",  Fake___system_property_get,  _Original___system_property_get);
        PATCH_ALIAS("__system_property_find", Fake___system_property_find, _Original___system_property_find);
        PATCH_ALIAS("__system_property_read_callback",
                    Fake___system_property_read_callback, _Original___system_property_read_callback);
        PATCH_ALIAS("__system_property_read", Fake___system_property_read, _Original___system_property_read);
    }

    if (ShouldHookDlopen(info->dlpi_name)) {
        return 0;
    }

    if (!ShouldHookFileIO(info->dlpi_name)) return 0;

    PATCH(open,       Fake_open);
    PATCH(open64,     Fake_open64);
    PATCH(openat,     Fake_openat);
    PATCH(openat64,   Fake_openat64);
    PATCH(stat,       Fake_stat);
    PATCH(lstat,      Fake_lstat);
    PATCH(stat64,     Fake_stat64);
    PATCH(lstat64,    Fake_lstat64);
    PATCH(fstatat,    Fake_fstatat);
    PATCH(statx,      Fake_statx);
    PATCH(access,     Fake_access);
    PATCH(faccessat,  Fake_faccessat);
    PATCH(readlink,   Fake_readlink);
    PATCH(unlink,     Fake_unlink);
    PATCH(unlinkat,   Fake_unlinkat);
    PATCH(rename,     Fake_rename);
    PATCH(renameat,   Fake_renameat);
    PATCH(mkdir,      Fake_mkdir);
    PATCH(mkdirat,    Fake_mkdirat);
    PATCH(rmdir,      Fake_rmdir);
    PATCH(fopen,      Fake_fopen);
    PATCH(fopen64,    Fake_fopen64);
    PATCH(opendir,    Fake_opendir);
    PATCH(readdir,    Fake_readdir);
    PATCH(readdir64,  Fake_readdir64);
    PATCH(getdents64, Fake_getdents64);
    PATCH(getuid,     Fake_getuid);
    PATCH(geteuid,    Fake_geteuid);
    PATCH(getgid,     Fake_getgid);
    PATCH(getegid,    Fake_getegid);

    PATCH_ALIAS("__open_2",    Fake__open_2,     _Original__open_2);
    PATCH_ALIAS("__openat_2",  Fake__openat_2,   _Original__openat_2);
    PATCH_ALIAS("__stat",      Fake___stat,      _Original___stat);
    PATCH_ALIAS("__lstat",     Fake___lstat,     _Original___lstat);
    PATCH_ALIAS("__stat64",    Fake___stat64,    _Original___stat64);
    PATCH_ALIAS("__fstatat64", Fake___fstatat64, _Original___fstatat64);

    return 0;
}

int InstallHooks() {
    std::lock_guard<std::mutex> lock(g_rescanMutex);
    InstallCtx ctx;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t sz, void* d) -> int {
        if (info->dlpi_name && info->dlpi_name[0])
            g_seenLibs.insert(info->dlpi_name);
        return HookLibraryCallback(info, sz, d);
    }, &ctx);

    if (ctx.failed) {
        LOGE("InstallHooks: failure at %s (r=%d) — rolling back %zu hooks",
             ctx.fail_where ? ctx.fail_where : "?", (int)ctx.error,
             ctx.installed.size());
        for (auto it = ctx.installed.rbegin(); it != ctx.installed.rend(); ++it) {
            HookResult ur = UnhookGotEntry((*it)->target);
            if (ur != HOOK_OK) {
                LOGE("InstallHooks rollback: failed to unhook %s (r=%d)",
                     (*it)->name, (int)ur);
            }
        }
        return (int)ctx.error;
    }

    LOGD("InstallHooks: done (%zu hooks, narrow scope)", ctx.installed.size());
    return HOOK_OK;
}

// ─────────────────────────────────────────────────────────────────────────
// Test helpers — item 2 (rollback). Walk the registry and unhook everything,
// so the rollback test can start from a known zero state.
// Not part of the production API; used only from test_item2.cpp.
// ─────────────────────────────────────────────────────────────────────────
extern "C" int UninstallAllHooks() {
    std::lock_guard<std::mutex> rescanLock(g_rescanMutex);
    HookHandle buf[512];
    int n = RegistrySnapshot(buf, 512);
    int failures = 0;
    for (int i = 0; i < n; i++) {
        HookResult r = HOOK_INTERNAL_ERROR;
        if (buf[i].type == HT_GOT) {
            r = UnhookGotEntry(buf[i].target);
        } else if (buf[i].type == HT_INLINE) {
            r = UninstallInlineHook(buf[i].target);
        }
        if (r != HOOK_OK && r != HOOK_NOT_HOOKED) {
            LOGE("UninstallAllHooks: %s failed r=%d", buf[i].name, (int)r);
            failures++;
        }
    }
    LOGD("UninstallAllHooks: %d hooks examined, %d failures", n, failures);
    return failures;
}
// ================= END OF FILE =================