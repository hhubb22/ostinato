set(OSTPROTO_PROTOS
    protocol.proto emulproto.proto mac.proto payload.proto eth2.proto dot3.proto
    llc.proto snap.proto dot2llc.proto dot2snap.proto vlan.proto svlan.proto
    vlanstack.proto stp.proto arp.proto ip4.proto ip6.proto ip6over4.proto
    ip4over6.proto ip4over4.proto ip6over6.proto gre.proto icmp.proto gmp.proto
    igmp.proto mld.proto tcp.proto udp.proto textproto.proto userscript.proto
    hexdump.proto sample.proto sign.proto)

set(_all_protos ${OSTPROTO_PROTOS} fileformat.proto)
set(_all_proto_paths)
foreach(proto IN LISTS _all_protos)
    list(APPEND _all_proto_paths "${CMAKE_CURRENT_SOURCE_DIR}/${proto}")
endforeach()

function(ostinato_generate_protos out_sources out_headers)
    set(generated_sources)
    set(generated_headers)
    foreach(proto IN LISTS ARGN)
        get_filename_component(base "${proto}" NAME_WE)
        set(source "${CMAKE_CURRENT_BINARY_DIR}/${base}.pb.cc")
        set(header "${CMAKE_CURRENT_BINARY_DIR}/${base}.pb.h")
        add_custom_command(
            OUTPUT "${source}" "${header}"
            COMMAND protobuf::protoc
                --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
                "${CMAKE_CURRENT_SOURCE_DIR}/${proto}"
            DEPENDS protobuf::protoc ${_all_proto_paths}
            COMMENT "Generating C++ protocol buffer ${proto}"
            VERBATIM)
        list(APPEND generated_sources "${source}")
        list(APPEND generated_headers "${header}")
    endforeach()
    set_source_files_properties(${generated_sources} ${generated_headers} PROPERTIES GENERATED TRUE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(${generated_sources} PROPERTIES COMPILE_OPTIONS "-Wno-error")
    endif()
    set(${out_sources} ${generated_sources} PARENT_SCOPE)
    set(${out_headers} ${generated_headers} PARENT_SCOPE)
endfunction()

if(_ostinato_build_any_target OR _ostinato_qtfree_drone
        OR (BUILD_TESTING AND OSTINATO_BUILD_CORE_TESTS))
    ostinato_generate_protos(ostproto_pb_sources ostproto_pb_headers ${OSTPROTO_PROTOS})
    add_custom_target(ostproto_generated_headers DEPENDS ${ostproto_pb_headers})
    add_library(ostproto_messages STATIC ${ostproto_pb_sources} ${ostproto_pb_headers})
    add_dependencies(ostproto_messages ostproto_generated_headers)
    target_include_directories(ostproto_messages PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(ostproto_messages PUBLIC protobuf::libprotobuf)
    ostinato_target_defaults(ostproto_messages)
endif()

set(OSTPROTO_CORE_SOURCES
    abstractprotocol.cpp crc32c.cpp protocolmanager.cpp protocollist.cpp
    protocollistiterator.cpp streambase.cpp updater.cpp mac.cpp vlan.cpp svlan.cpp
    eth2.cpp dot3.cpp llc.cpp snap.cpp stp.cpp arp.cpp ip4.cpp ip6.cpp gmp.cpp
    gre.cpp icmp.cpp igmp.cpp mld.cpp tcp.cpp udp.cpp textproto.cpp hexdump.cpp
    packet.cpp payload.cpp sample.cpp sign.cpp userscript.cpp)

if(_ostinato_qtfree_drone OR (BUILD_TESTING AND OSTINATO_BUILD_CORE_TESTS))
list(REMOVE_ITEM OSTPROTO_CORE_SOURCES updater.cpp)
add_library(ostproto_core STATIC
    ${OSTPROTO_CORE_SOURCES} userscript.h)
set_property(TARGET ostproto_core PROPERTY CXX_STANDARD 17)
target_include_directories(ostproto_core BEFORE PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/qtfree"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ostproto_core PUBLIC QuickJS::QuickJS ostproto_messages)
ostinato_target_defaults(ostproto_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ostproto_core PRIVATE -Wno-cast-function-type)
endif()
endif()

if(_ostinato_build_any_target)
add_library(ostproto STATIC
    ${OSTPROTO_CORE_SOURCES} updater.cpp updater.h userscript.h)
target_include_directories(ostproto PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ostproto PUBLIC
    Qt5::Core Qt5::Network Qt5::Xml QuickJS::QuickJS ostproto_messages)
ostinato_target_defaults(ostproto)
ostinato_qt_target(ostproto)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ostproto PRIVATE -Wno-cast-function-type)
endif()

endif()

if(_ostinato_build_any_target)
ostinato_generate_protos(ostfile_pb_sources ostfile_pb_headers fileformat.proto)
add_library(ostfile STATIC
    ostprotolib.cpp nativefileformat.cpp ossnfileformat.cpp ostmfileformat.cpp
    pcapfileformat.cpp pdmlfileformat.cpp pythonfileformat.cpp pdmlprotocol.cpp
    pdmlprotocols.cpp pdmlreader.cpp sessionfileformat.cpp streamfileformat.cpp
    vlanpdml.cpp svlanpdml.cpp stppdml.cpp eth2pdml.cpp llcpdml.cpp arppdml.cpp
    ip4pdml.cpp ip6pdml.cpp grepdml.cpp icmppdml.cpp icmp6pdml.cpp igmppdml.cpp
    mldpdml.cpp tcppdml.cpp udppdml.cpp textprotopdml.cpp samplepdml.cpp
    pdmlreader.h sessionfileformat.h streamfileformat.h
    ${ostfile_pb_sources} ${ostfile_pb_headers})
target_include_directories(ostfile PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ostfile PUBLIC ostproto Qt5::Network Qt5::Xml protobuf::libprotobuf)
ostinato_target_defaults(ostfile)
ostinato_qt_target(ostfile)

if(OSTINATO_BUILD_CLIENT)
add_library(ostfilegui STATIC
    fileformatoptions.cpp pcapoptionsdialog.cpp pcapfileimport.ui fileformatoptions.h)
target_include_directories(ostfilegui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ostfilegui PUBLIC ostfile Qt5::Widgets)
ostinato_qt_target(ostfilegui)

add_library(ostprotogui STATIC
    spinboxdelegate.cpp tosdscp.cpp protocolwidgetfactory.cpp macconfig.cpp
    vlanconfig.cpp eth2config.cpp dot3config.cpp llcconfig.cpp snapconfig.cpp
    stpconfig.cpp arpconfig.cpp ip4config.cpp ip6config.cpp gmpconfig.cpp
    greconfig.cpp icmpconfig.cpp igmpconfig.cpp mldconfig.cpp tcpconfig.cpp
    udpconfig.cpp textprotoconfig.cpp hexdumpconfig.cpp payloadconfig.cpp
    sampleconfig.cpp signconfig.cpp userscriptconfig.cpp
    mac.ui vlan.ui eth2.ui dot3.ui llc.ui snap.ui stp.ui arp.ui ip4.ui ip6.ui
    gmp.ui gre.ui icmp.ui tcp.ui udp.ui textproto.ui tosdscp.ui hexdump.ui
    payload.ui sample.ui sign.ui userscript.ui
    abstractprotocolconfig.h arpconfig.h dot3config.h eth2config.h gmpconfig.h
    greconfig.h hexdumpconfig.h icmpconfig.h igmpconfig.h ip4config.h ip6config.h
    ipv4addressdelegate.h ipv6addressdelegate.h llcconfig.h macconfig.h mldconfig.h
    payloadconfig.h sampleconfig.h signconfig.h snapconfig.h spinboxdelegate.h
    stpconfig.h tcpconfig.h textprotoconfig.h tosdscp.h udpconfig.h
    userscriptconfig.h vlanconfig.h)
target_include_directories(ostprotogui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ostprotogui PUBLIC
    ostproto qhexedit2 Qt5::Network Qt5::Widgets Qt5::Xml)
ostinato_target_defaults(ostprotogui)
ostinato_qt_target(ostprotogui)
endif()
endif()
