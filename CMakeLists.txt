cmake_minimum_required(VERSION 3.16)

project(Ostinato VERSION 1.4.0 LANGUAGES C CXX)

include(CTest)

option(OSTINATO_BUILD_CLIENT "Build the Ostinato GUI client" ON)
option(OSTINATO_BUILD_DRONE "Build the Ostinato drone agent" ON)
option(OSTINATO_BUILD_QT_TESTS "Build the Qt-based tests and test tools" ON)
set(_ostinato_core_tests_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ostinato_core_tests_default ON)
endif()
option(OSTINATO_BUILD_CORE_TESTS "Build the portable packet/RPC core tests"
       ${_ostinato_core_tests_default})

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

set(_ostinato_build_qt_tests OFF)
if(BUILD_TESTING AND OSTINATO_BUILD_QT_TESTS)
    set(_ostinato_build_qt_tests ON)
endif()
set(_ostinato_qtfree_drone OFF)
if(OSTINATO_BUILD_DRONE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ostinato_qtfree_drone ON)
endif()
set(_ostinato_build_any_target OFF)
if(OSTINATO_BUILD_CLIENT OR _ostinato_build_qt_tests
        OR (OSTINATO_BUILD_DRONE AND NOT _ostinato_qtfree_drone))
    set(_ostinato_build_any_target ON)
endif()

find_package(Protobuf REQUIRED)

if(_ostinato_build_any_target)
    set(_ostinato_qt_components Core Network Xml)
    if(OSTINATO_BUILD_CLIENT)
        list(APPEND _ostinato_qt_components Gui Svg Widgets)
    endif()
    if(OSTINATO_BUILD_CLIENT OR _ostinato_build_qt_tests)
        list(APPEND _ostinato_qt_components Test)
    endif()
    list(REMOVE_DUPLICATES _ostinato_qt_components)
    find_package(Qt5 5.7 REQUIRED COMPONENTS ${_ostinato_qt_components})
endif()

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

function(ostinato_make_quickjs_available)
    # QuickJS-NG declares CMake 3.10, so set the default for CMP0077 in its
    # policy scope and make its untyped qjs library ignore the parent setting.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(quickjs)
endfunction()
ostinato_make_quickjs_available()

get_target_property(_quickjs_type qjs TYPE)
if(NOT _quickjs_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Pinned QuickJS target must be static, got ${_quickjs_type}")
endif()
set_target_properties(
    qjsc qjs_exe qjs-libc run-test262 api-test lre-test function_source
    PROPERTIES
    EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
add_library(QuickJS::QuickJS ALIAS qjs)

function(ostinato_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_11)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Debug>:-Wall;-W;-Wextra;-Werror;-Wno-deprecated-declarations>)
    endif()
endfunction()

function(ostinato_qt_target target)
    set_target_properties(${target} PROPERTIES
        AUTOMOC ON
        AUTOUIC ON
        AUTORCC ON)
    if(MSVC)
        # Qt maps this to MSVC's __FUNCSIG__, matching GCC/Clang diagnostics.
        target_compile_definitions(${target} PRIVATE __PRETTY_FUNCTION__=Q_FUNC_INFO)
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

add_subdirectory(common)
add_subdirectory(rpc)
if(TARGET ostproto_generated_headers)
    add_library(ostinato_client_domain STATIC
        client/core/port_state.cpp client/core/port_state.h)
    add_dependencies(ostinato_client_domain ostproto_generated_headers)
    target_include_directories(ostinato_client_domain PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/client/core"
        "${CMAKE_CURRENT_BINARY_DIR}/common")
    target_link_libraries(ostinato_client_domain PUBLIC ostproto_messages)
    ostinato_target_defaults(ostinato_client_domain)
endif()
if(UNIX AND TARGET ostinato_client_domain AND TARGET pbrpc_client_core)
    add_library(ostinato_client_core STATIC
        client/core/client_session.cpp client/core/client_session.h)
    add_dependencies(ostinato_client_core ostproto_generated_headers)
    target_include_directories(ostinato_client_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/client/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/rpc"
        "${CMAKE_CURRENT_BINARY_DIR}/common")
    target_link_libraries(ostinato_client_core PUBLIC
        ostinato_client_domain pbrpc_client_core)
    ostinato_target_defaults(ostinato_client_core)
endif()
if(UNIX AND TARGET ostinato_client_core)
    add_executable(ostinato-controller
        controller/main.cpp
        controller/stdio_protocol.cpp controller/stdio_protocol.h)
    set_property(TARGET ostinato-controller PROPERTY CXX_STANDARD 17)
    target_include_directories(ostinato-controller PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/controller")
    target_link_libraries(ostinato-controller PRIVATE
        ostinato_client_core protobuf::libprotobuf)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The desktop package places its private runtime closure beside the
        # controller. Development still falls back to system libraries.
        set_target_properties(ostinato-controller PROPERTIES
            BUILD_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN")
    endif()
    ostinato_target_defaults(ostinato-controller)
    install(TARGETS ostinato-controller RUNTIME DESTINATION bin)
endif()
if(BUILD_TESTING AND OSTINATO_BUILD_CORE_TESTS)
    add_executable(ostproto_core_tests test/ostproto_core_tests.cpp)
    set_property(TARGET ostproto_core_tests PROPERTY CXX_STANDARD 17)
    target_link_libraries(ostproto_core_tests PRIVATE ostproto_core)
    ostinato_target_defaults(ostproto_core_tests)
    add_test(NAME ostproto_core COMMAND ostproto_core_tests)
    add_executable(pbrpc_core_tests test/pbrpc_core_tests.cpp)
    target_link_libraries(pbrpc_core_tests PRIVATE pbrpc_core)
    ostinato_target_defaults(pbrpc_core_tests)
    add_test(NAME pbrpc_core COMMAND pbrpc_core_tests)
    if(TARGET pbrpc_server_core)
        protobuf_generate_cpp(pbrpc_server_test_sources pbrpc_server_test_headers
            test/pbrpc_server_test.proto)
        add_executable(pbrpc_server_core_tests test/pbrpc_server_core_tests.cpp
            ${pbrpc_server_test_sources} ${pbrpc_server_test_headers})
        target_include_directories(pbrpc_server_core_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(pbrpc_server_core_tests PRIVATE pbrpc_server_core)
        ostinato_target_defaults(pbrpc_server_core_tests)
        add_test(NAME pbrpc_server_core COMMAND pbrpc_server_core_tests)
        if(TARGET pbrpc_client_core)
            add_executable(pbrpc_client_core_tests test/pbrpc_client_core_tests.cpp
                ${pbrpc_server_test_sources} ${pbrpc_server_test_headers})
            target_include_directories(pbrpc_client_core_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
            target_link_libraries(pbrpc_client_core_tests PRIVATE
                pbrpc_client_core_testing pbrpc_server_core)
            target_compile_definitions(pbrpc_client_core_tests PRIVATE
                PBRPC_CLIENT_CORE_TESTING)
            ostinato_target_defaults(pbrpc_client_core_tests)
            add_test(NAME pbrpc_client_core COMMAND pbrpc_client_core_tests)
            if(TARGET ostinato_client_core)
                add_executable(headless_client_e2e_tests
                    test/headless_client_e2e_tests.cpp)
                target_link_libraries(headless_client_e2e_tests PRIVATE
                    ostinato_client_core pbrpc_server_core)
                ostinato_target_defaults(headless_client_e2e_tests)
                add_test(NAME headless_client_e2e COMMAND headless_client_e2e_tests)
            endif()
        endif()
    endif()
    if(TARGET ostinato-controller)
        add_executable(controller_protocol_tests
            test/controller_protocol_tests.cpp controller/stdio_protocol.cpp)
        set_property(TARGET controller_protocol_tests PROPERTY CXX_STANDARD 17)
        target_include_directories(controller_protocol_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/controller")
        ostinato_target_defaults(controller_protocol_tests)
        add_test(NAME controller_protocol COMMAND controller_protocol_tests)

        add_executable(controller_process_tests
            test/controller_process_tests.cpp controller/stdio_protocol.cpp)
        set_property(TARGET controller_process_tests PROPERTY CXX_STANDARD 17)
        target_include_directories(controller_process_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/controller")
        target_compile_definitions(controller_process_tests PRIVATE
            OSTINATO_CONTROLLER_PATH="$<TARGET_FILE:ostinato-controller>")
        add_dependencies(controller_process_tests ostinato-controller)
        target_link_libraries(controller_process_tests PRIVATE
            pbrpc_server_core ostproto_messages)
        ostinato_target_defaults(controller_process_tests)
        add_test(NAME controller_process COMMAND controller_process_tests)
    endif()
endif()
if(_ostinato_build_any_target)
    if(OSTINATO_BUILD_CLIENT)
        add_subdirectory(extra)
    endif()
endif()
if(OSTINATO_BUILD_DRONE)
    add_subdirectory(server)
endif()
if(UNIX AND BUILD_TESTING AND OSTINATO_BUILD_CORE_TESTS
        AND TARGET drone AND TARGET ostinato_client_core)
    add_executable(headless_client_drone_tests
        test/headless_client_drone_tests.cpp)
    target_compile_definitions(headless_client_drone_tests PRIVATE
        OSTINATO_DRONE_PATH="$<TARGET_FILE:drone>")
    target_link_libraries(headless_client_drone_tests PRIVATE ostinato_client_core)
    add_dependencies(headless_client_drone_tests drone)
    ostinato_target_defaults(headless_client_drone_tests)
    add_test(NAME headless_client_drone COMMAND headless_client_drone_tests)
endif()
if(OSTINATO_BUILD_CLIENT)
    add_subdirectory(client)
endif()
if(_ostinato_build_qt_tests)
    add_subdirectory(test)
endif()
