cmake_minimum_required(VERSION 3.22.1)


project("hookengine")


add_library(${CMAKE_PROJECT_NAME} SHARED
        native-lib.cpp
        elf_hooker.cpp
        RedirectTable.cpp
        Hooks.cpp
        InlineHooker.cpp
        HiddenApi.cpp
)

find_library(
        log-lib
        log
)


target_link_libraries(${CMAKE_PROJECT_NAME}
        android
        log)