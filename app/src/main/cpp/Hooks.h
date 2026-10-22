#pragma once
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <linux/stat.h>
#include <dirent.h>

// Shared virtual UID/GID storage (defined in Hooks.cpp)
extern int32_t g_virtualUid;
extern int32_t g_virtualGid;
void SetVirtualUid(int32_t uid);

// ---- File I/O originals ----
extern int (*_Original_open)(const char*, int, ...);
extern int (*_Original_open64)(const char*, int, ...);
extern int (*_Original__open_2)(const char*, int);
extern int (*_Original_openat)(int, const char*, int, ...);
extern int (*_Original_openat64)(int, const char*, int, ...);
extern int (*_Original__openat_2)(int, const char*, int);
extern int (*_Original_stat)(const char*, struct stat*);
extern int (*_Original_lstat)(const char*, struct stat*);
extern int (*_Original_stat64)(const char*, struct stat64*);
extern int (*_Original_lstat64)(const char*, struct stat64*);
extern int (*_Original___stat)(const char*, struct stat*);
extern int (*_Original___lstat)(const char*, struct stat*);
extern int (*_Original___stat64)(const char*, struct stat64*);
extern int (*_Original___fstatat64)(int, const char*, struct stat*, int);
extern int (*_Original_statx)(int, const char*, int, unsigned int, struct statx*);
extern int (*_Original_fstatat)(int, const char*, struct stat*, int);
extern int (*_Original_access)(const char*, int);
extern int (*_Original_faccessat)(int, const char*, int, int);
extern ssize_t (*_Original_readlink)(const char*, char*, size_t);
extern int (*_Original_unlink)(const char*);
extern int (*_Original_unlinkat)(int, const char*, int);
extern int (*_Original_rename)(const char*, const char*);
extern int (*_Original_renameat)(int, const char*, int, const char*);
extern int (*_Original_mkdir)(const char*, mode_t);
extern int (*_Original_mkdirat)(int, const char*, mode_t);
extern int (*_Original_rmdir)(const char*);
extern FILE* (*_Original_fopen)(const char*, const char*);
extern FILE* (*_Original_fopen64)(const char*, const char*);
extern DIR*  (*_Original_opendir)(const char*);
extern struct dirent*   (*_Original_readdir)(DIR*);
extern struct dirent64* (*_Original_readdir64)(DIR*);
extern int (*_Original_getdents64)(unsigned int, struct dirent64*, unsigned int);

// ---- Loader ----
extern void* (*_Original_dlopen)(const char*, int);
extern void* (*_Original_android_dlopen_ext)(const char*, int, const void*);

// ---- Property API ----
extern int   (*_Original___system_property_get)(const char*, char*);
extern const void* (*_Original___system_property_find)(const char*);
extern void  (*_Original___system_property_read_callback)(
        const void*, void(*)(void*, const char*, const char*, uint32_t), void*);
extern int   (*_Original___system_property_read)(const void*, char*, char*);

// ---- UID / GID ----
extern uid_t (*_Original_getuid)();
extern uid_t (*_Original_geteuid)();
extern gid_t (*_Original_getgid)();
extern gid_t (*_Original_getegid)();

void InstallHooks();