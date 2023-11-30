cmake_minimum_required(VERSION 3.18)

project(Hook
    VERSION 0.1.1
    LANGUAGES CXX OBJCXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wformat")

find_package(Git REQUIRED)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
   option(GIT_SUBMODULE "Check submodules during build" ON)
   if(GIT_SUBMODULE)
       message(STATUS "Submodule update")
       execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                       RESULT_VARIABLE GIT_SUBMOD_RESULT)
       if(NOT GIT_SUBMOD_RESULT EQUAL "0")
           message(FATAL_ERROR "git submodule update --init failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
       endif()
   endif()
endif()

include(ExternalProject)

ExternalProject_Add(${PROJECT_NAME}-llvm
    SOURCE_DIR          ${CMAKE_CURRENT_SOURCE_DIR}/external/llvm-project/llvm
    BINARY_DIR          ${PROJECT_BINARY_DIR}/llvm-build
    CONFIGURE_COMMAND   ${CMAKE_COMMAND}
        -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        -DLLVM_ENABLE_PROJECTS=clang
        -DLLVM_ENABLE_TERMINFO=OFF
        -DLLVM_ENABLE_IDE=OFF
        -DLLVM_ENABLE_LIBXML2=OFF
        -DLLVM_ENABLE_LIBEDIT=OFF
        -DLLVM_TARGETS_TO_BUILD=ARM
        ${CMAKE_CURRENT_SOURCE_DIR}/external/llvm-project/llvm
    BUILD_COMMAND       ninja
    INSTALL_COMMAND     ""
    TEST_COMMAND        ""
    STEP_TARGETS        build
)

ExternalProject_Add(${PROJECT_NAME}-lldb
    SOURCE_DIR          ${CMAKE_CURRENT_SOURCE_DIR}/external/llvm-project/lldb
    BINARY_DIR          ${PROJECT_BINARY_DIR}/lldb-build
    CONFIGURE_COMMAND   ${CMAKE_COMMAND}
        -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        -DLLVM_DIR=${PROJECT_BINARY_DIR}/llvm-build/lib/cmake/llvm
        -DLLDB_INCLUDE_TESTS=OFF
        -DLLDB_ENABLE_PYTHON=OFF
        -DLLDB_ENABLE_CURSES=OFF
        -DLLDB_ENABLE_TERMIOS=OFF
        -DLLDB_ENABLE_LIBEDIT=OFF
        -DLLDB_ENABLE_LIBXML2=OFF
        ${CMAKE_CURRENT_SOURCE_DIR}/external/llvm-project/lldb
    BUILD_COMMAND       ninja lldb debugserver
    INSTALL_COMMAND     ""
    TEST_COMMAND        ""
    DEPENDS             ${PROJECT_NAME}-llvm-build
)

add_subdirectory(external/glfw)

set(CPP_SOURCES
    src/main.cpp
    external/imgui/imgui.cpp
    external/imgui/misc/cpp/imgui_stdlib.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
)

set(OBJC_SOURCES
    external/imgui/backends/imgui_impl_metal.mm
    src/backend.cpp
)

set(APP_ICON_MACOSX resources/icons/hook.icns)

set_source_files_properties(${APP_ICON_MACOSX} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

add_executable(${PROJECT_NAME} MACOSX_BUNDLE
    ${CPP_SOURCES}
    ${OBJC_SOURCES}
    ${APP_ICON_MACOSX}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_ICON_FILE hook.icns
)

configure_file(src/config.h.in config.h)

target_include_directories(${PROJECT_NAME}
    PRIVATE ${PROJECT_BINARY_DIR}
    PRIVATE external/glfw/include
    PRIVATE external/imgui
    PRIVATE external/imgui/backends
    PRIVATE external/imgui/misc/cpp
    PRIVATE external/llvm-project/lldb/include
    PRIVATE src
)

target_link_directories(${PROJECT_NAME}
    PRIVATE ${PROJECT_BINARY_DIR}/lldb-build/lib
)

target_link_libraries(${PROJECT_NAME}
    "lldb"
    "glfw"
    "-framework Metal"
    "-framework MetalKit"
    "-framework Cocoa"
    "-framework IOKit"
    "-framework CoreVideo"
    "-framework QuartzCore"
    "-framework Security"
)

set_source_files_properties(${OBJC_SOURCES} PROPERTIES
    LANGUAGE OBJCXX
    COMPILE_FLAGS "-x objective-c++"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${PROJECT_NAME}>/../Frameworks
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/lldb-build/bin/debugserver $<TARGET_FILE_DIR:${PROJECT_NAME}>/../Frameworks/
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/lldb-build/lib/liblldb.17.0.4.dylib $<TARGET_FILE_DIR:${PROJECT_NAME}>/../Frameworks/
    COMMAND ${CMAKE_INSTALL_NAME_TOOL} -change @rpath/liblldb.17.0.4.dylib @executable_path/../Frameworks/liblldb.17.0.4.dylib $<TARGET_FILE:${PROJECT_NAME}>
)