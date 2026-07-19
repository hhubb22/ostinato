add_library(pbrpc_core STATIC pbrpc_core.cpp pbrpc_core.h)
target_include_directories(pbrpc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
ostinato_target_defaults(pbrpc_core)

add_library(pbrpc_server_core STATIC pbrpc_server_core.cpp pbrpc_server_core.h)
target_include_directories(pbrpc_server_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pbrpc_server_core PUBLIC pbrpc_core protobuf::libprotobuf)
if(UNIX)
    target_link_libraries(pbrpc_server_core PUBLIC pthread)
endif()
ostinato_target_defaults(pbrpc_server_core)

if(_ostinato_build_any_target)
add_library(pbrpc STATIC
    rpcserver.cpp rpcconn.cpp pbrpcchannel.cpp versioncompatibility.cpp
    rpcserver.h rpcconn.h pbrpcchannel.h versioncompatibility.h)
target_compile_definitions(pbrpc PUBLIC HAVE_REMOTE)
target_include_directories(pbrpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pbrpc PUBLIC pbrpc_core Qt5::Network protobuf::libprotobuf)
ostinato_target_defaults(pbrpc)
ostinato_qt_target(pbrpc)
endif()
