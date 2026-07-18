add_library(pbrpc STATIC
    rpcserver.cpp rpcconn.cpp pbrpcchannel.cpp
    rpcserver.h rpcconn.h pbrpcchannel.h)
target_compile_definitions(pbrpc PUBLIC HAVE_REMOTE)
target_include_directories(pbrpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pbrpc PUBLIC Qt5::Network protobuf::libprotobuf)
ostinato_target_defaults(pbrpc)
