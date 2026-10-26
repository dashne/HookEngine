cmake_minimum_required(VERSION 3.22.1)

project("hookengine")

add_library(${CMAKE_PROJECT_NAME} SHARED
        native-lib.cpp
        elf_hooker.cpp
        RedirectTable.cpp
        Hooks.cpp
        InlineHooker.cpp
        HiddenApi.cpp
        HookRegistry.cpp
        arm64_reloc.cpp
        JavaHook.cpp
        ProcessHooks.cpp
        DualApplication.cpp
        AntiDetection.cpp
        BinderProbe.cpp
        BinderTransactHook.cpp
        CppParcelRewriter.cpp
        ProxyBridge.cpp
        ChainHook.cpp
        ArtMethodTrace.cpp
        ReturnMod.cpp
        SvcHook.cpp
        RegInspect.cpp
        MemoryOps.cpp
        MapsEnum.cpp
        ApplicationProxy.cpp
        GuestBoot.cpp
        test_item1.cpp
        test_item2.cpp
        test_item3.cpp
        test_item4.cpp
        test_item5.cpp
        test_suite.cpp
        test_regression.cpp
        test_reloc.cpp
        test_process_hooks.cpp
        test_anti_detection.cpp
)

find_library(log-lib log)

target_link_libraries(${CMAKE_PROJECT_NAME}
        android
        log)