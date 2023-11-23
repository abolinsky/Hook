cmake_minimum_required(VERSION 3.15)

project(hook)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_language(OBJCXX)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wformat")

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/dependencies/imgui")
include_directories(${IMGUI_DIR})
include_directories(${IMGUI_DIR}/backends)
include_directories(/usr/local/include)
include_directories(.)

find_library(LLDB_LIB lldb REQUIRED PATHS /usr/local/lib/lldb)
find_package(glfw3 REQUIRED)

set(SOURCES
    main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_stdlib.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
)

set(OBJC_SOURCES
    ${IMGUI_DIR}/backends/imgui_impl_metal.mm
    backend.cpp
)

add_executable(hook ${SOURCES} ${OBJC_SOURCES})

target_link_libraries(hook ${LLDB_LIB} glfw "-framework Metal" "-framework MetalKit" "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework QuartzCore")

set_target_properties(hook PROPERTIES INSTALL_RPATH "/usr/local/lib/lldb")

set_source_files_properties(${OBJC_SOURCES} PROPERTIES LANGUAGE OBJCXX)
set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_metal.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE PATH "Default install path" FORCE)
endif()

install(TARGETS hook DESTINATION bin)