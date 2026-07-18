cmake_minimum_required(VERSION 3.16)

project(Ostinato VERSION 1.4.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    add_link_options(/STACK:8388608)
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(APPLE)
        set(CMAKE_INSTALL_PREFIX "/Applications" CACHE PATH "Install prefix" FORCE)
    elseif(WIN32)
        get_filename_component(_default_windows_prefix "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
        set(CMAKE_INSTALL_PREFIX "${_default_windows_prefix}" CACHE PATH "Install prefix" FORCE)
    endif()
endif()

find_package(Qt5 5.7 REQUIRED COMPONENTS
    Core Gui Network Svg Test Widgets Xml)
find_package(Protobuf REQUIRED)

option(OSTINATO_USE_SYSTEM_QUICKJS "Use an externally built QuickJS library" OFF)
if(OSTINATO_USE_SYSTEM_QUICKJS)
    find_path(QUICKJS_INCLUDE_DIR quickjs.h REQUIRED)
    find_library(QUICKJS_LIBRARY NAMES quickjs qjs REQUIRED)
    add_library(QuickJS::QuickJS UNKNOWN IMPORTED)
    set_target_properties(QuickJS::QuickJS PROPERTIES
        IMPORTED_LOCATION "${QUICKJS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${QUICKJS_INCLUDE_DIR}")
else()
    include(FetchContent)
    set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
    set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(QJS_BUILD_WERROR OFF CACHE BOOL "" FORCE)
    set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(quickjs
        URL https://github.com/quickjs-ng/quickjs/archive/fd0a0210b7be00957751871e7e01b8291268fc29.tar.gz
        URL_HASH SHA256=39d931489e99f80b496f900f88d67d7aad6b5f25ce83810a408eb42f6af6839e)
    FetchContent_MakeAvailable(quickjs)
    set_target_properties(
        qjsc qjs_exe qjs-libc run-test262 api-test lre-test function_source
        PROPERTIES
        EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
    add_library(QuickJS::QuickJS ALIAS qjs)
endif()

function(ostinato_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_11)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Debug>:-Wall;-W;-Wextra;-Werror;-Wno-deprecated-declarations>)
    endif()
endfunction()

execute_process(
    COMMAND git rev-parse --short=12 --verify HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE OSTINATO_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT OSTINATO_REVISION)
    set(OSTINATO_REVISION "unknown")
endif()
set(OSTINATO_VERSION "${PROJECT_VERSION}-dev")

function(ostinato_add_version_source target)
    set(version_source "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.cpp")
    file(WRITE "${version_source}"
        "const char *version = \"${OSTINATO_VERSION}\";\n"
        "const char *revision = \"${OSTINATO_REVISION}\";\n")
    target_sources(${target} PRIVATE "${version_source}")
endfunction()

add_subdirectory(extra)
add_subdirectory(common)
add_subdirectory(rpc)
add_subdirectory(server)
add_subdirectory(client)
add_subdirectory(test)
