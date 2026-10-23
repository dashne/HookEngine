#include "AntiDetection.h"
#include "HookRegistry.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <android/log.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstring>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);

typedef int (*connect_t)(int, const struct sockaddr*, socklen_t);
static connect_t _connect = nullptr;
static void*    g_connect_target = nullptr;

// Debugging-tool ports we refuse. All confirmed in the wild:
//   27042/27043  Frida default frida-server
//   23946        IDA Pro remote debugger
//   27052        Frida newer default (post-14.x)
//   11111        objection default
static bool IsBlockedPort(uint16_t port) {
    switch (port) {
        case 27042: case 27043: case 23946: case 27052: case 11111:
            return true;
    }
    return false;
}

static bool ShouldBlockConnect(const struct sockaddr* addr) {
    if (!addr) return false;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)addr;
        uint16_t port = ntohs(sin->sin_port);
        uint32_t ip   = ntohl(sin->sin_addr.s_addr);
        if (ip == 0x7F000001 && IsBlockedPort(port)) {  // 127.0.0.1
            LOGD("connect BLOCKED: 127.0.0.1:%u", (unsigned)port);
            return true;
        }
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)addr;
        uint16_t port = ntohs(sin6->sin6_port);
        static const uint8_t loopback6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (memcmp(&sin6->sin6_addr, loopback6, 16) == 0 && IsBlockedPort(port)) {
            LOGD("connect BLOCKED: [::1]:%u", (unsigned)port);
            return true;
        }
    }
    return false;
}

static int Fake_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    if (ShouldBlockConnect(addr)) {
        errno = ECONNREFUSED;
        return -1;
    }
    if (!_connect) { errno = ENOSYS; return -1; }
    return _connect(sockfd, addr, addrlen);
}

extern "C" int InstallAntiDetection(JNIEnv*) {
    int n = 0;
    if (!g_connect_target) {
        void* t = dlsym(RTLD_DEFAULT, "connect");
        if (!t) { LOGE("InstallAntiDetection: dlsym(connect) failed"); return 0; }
        HookResult r = InstallInlineHookEx(t, (void*)Fake_connect,
                                           (void**)&_connect, "connect");
        if (r == HOOK_OK || r == HOOK_ALREADY_HOOKED) {
            g_connect_target = t;
            n++;
            LOGD("InstallAntiDetection: connect installed");
        } else {
            LOGE("InstallAntiDetection: install connect failed r=%d", (int)r);
        }
    } else {
        n++;
    }
    LOGD("InstallAntiDetection: %d hooks installed", n);
    return n;
}

extern "C" int UninstallAntiDetection() {
    if (!g_connect_target) return 0;
    HookResult r = UninstallInlineHook(g_connect_target);
    if (r == HOOK_OK || r == HOOK_NOT_HOOKED) {
        g_connect_target = nullptr;
        _connect = nullptr;
        LOGD("UninstallAntiDetection: connect removed");
        return 1;
    }
    LOGE("UninstallAntiDetection: failed r=%d", (int)r);
    return 0;
}

extern "C" int ShouldBlockConnectForTest(const char* ip, int port) {
    if (!ip) return 0;
    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) return 0;
    return ShouldBlockConnect((struct sockaddr*)&sa) ? 1 : 0;
}

extern "C" int IsConnectHookInstalled() {
    return (g_connect_target != nullptr && _connect != nullptr) ? 1 : 0;
}