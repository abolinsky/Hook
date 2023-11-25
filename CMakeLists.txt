cmake_minimum_required(VERSION 3.15)

project(hook)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_language(OBJCXX)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wformat")

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/dependencies/imgui")
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${IMGUI_DIR})
include_directories(${IMGUI_DIR}/backends)
include_directories(${CMAKE_INCLUDE_PATH})

find_library(LLDB_LIB lldb REQUIRED PATHS ${CMAKE_LIBRARY_PATH})
find_package(glfw3 REQUIRED)

set(SOURCES
    src/main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_stdlib.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
)

set(OBJC_SOURCES
    ${IMGUI_DIR}/backends/imgui_impl_metal.mm
    src/backend.cpp
)

set(APP_ICON_MACOSX resources/icons/hook.icns)
set_source_files_properties(${APP_ICON_MACOSX} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

add_executable(hook MACOSX_BUNDLE ${SOURCES} ${OBJC_SOURCES} ${APP_ICON_MACOSX})

set_target_properties(hook PROPERTIES
    MACOSX_BUNDLE_ICON_FILE hook.icns
)

target_link_libraries(hook ${LLDB_LIB} glfw "-framework Metal" "-framework MetalKit" "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework QuartzCore")

set_source_files_properties(${OBJC_SOURCES} PROPERTIES LANGUAGE OBJCXX)
set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_metal.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")

install(TARGETS hook
    BUNDLE DESTINATION /Applications
    COMPONENT Applications)