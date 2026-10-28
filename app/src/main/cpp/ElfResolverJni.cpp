#include <jni.h>
#include <cstdint>
#include "ElfResolver.h"

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfSymbolLookup(
        JNIEnv* env, jobject, jstring jlib, jstring jsym) {
    if (!env || !jlib || !jsym) return 0;
    const char* lib = env->GetStringUTFChars(jlib, nullptr);
    const char* sym = env->GetStringUTFChars(jsym, nullptr);
    if (!lib || !sym) {
        if (lib) env->ReleaseStringUTFChars(jlib, lib);
        if (sym) env->ReleaseStringUTFChars(jsym, sym);
        return 0;
    }
    void* addr = ElfSymbolLookup(lib, sym);
    env->ReleaseStringUTFChars(jlib, lib);
    env->ReleaseStringUTFChars(jsym, sym);
    return (jlong)(uintptr_t)addr;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfSymbolLookupMulti(
        JNIEnv* env, jobject, jstring jsym) {
    if (!env || !jsym) return 0;
    const char* sym = env->GetStringUTFChars(jsym, nullptr);
    if (!sym) return 0;
    void* addr = ElfSymbolLookupMulti(sym);
    env->ReleaseStringUTFChars(jsym, sym);
    return (jlong)(uintptr_t)addr;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfSymbolLookupLastPath(
        JNIEnv*, jobject) {
    return (jint)ElfSymbolLookupLastPath();
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfMapOpen(
        JNIEnv* env, jobject, jstring jlib) {
    if (!env || !jlib) return 0;
    const char* lib = env->GetStringUTFChars(jlib, nullptr);
    if (!lib) return 0;
    ElfMappedLib* m = ElfMappedLib_Open(lib);
    env->ReleaseStringUTFChars(jlib, lib);
    return (jlong)(uintptr_t)m;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfMapClose(
        JNIEnv*, jobject, jlong handle) {
    if (handle) ElfMappedLib_Close((ElfMappedLib*)(uintptr_t)handle);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfMapSectionCount(
        JNIEnv*, jobject, jlong handle) {
    if (!handle) return -1;
    return (jint)((ElfMappedLib*)(uintptr_t)handle)->shnum;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfSectionPresent(
        JNIEnv* env, jobject, jlong handle, jstring jname) {
    if (!handle || !jname) return -1;
    const ElfMappedLib* lib = (const ElfMappedLib*)(uintptr_t)handle;
    const char* name = env->GetStringUTFChars(jname, nullptr);
    if (!name) return -1;
    int present = ElfMappedLib_FindSection(lib, name) ? 1 : 0;
    env->ReleaseStringUTFChars(jname, name);
    return (jint)present;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfSymtabCount(
        JNIEnv*, jobject, jlong handle) {
    if (!handle) return -1;
    const ElfMappedLib* lib = (const ElfMappedLib*)(uintptr_t)handle;
    const Elf64_Shdr* hdr = ElfMappedLib_FindSection(lib, ".symtab");
    if (!hdr) return 0;
    return (jint)(hdr->sh_size / sizeof(Elf64_Sym));
}

// S68 revision 3 — enumerate .symtab names.
extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_NativeTestBridge_elfSymtabNthName(
        JNIEnv* env, jobject, jlong handle, jint idx) {
    if (!handle || idx <= 0) return nullptr;
    const char* name = ElfSymtabNthName((const ElfMappedLib*)(uintptr_t)handle,
                                        (size_t)idx);
    if (!name) return nullptr;
    return env->NewStringUTF(name);
}