include(CheckCXXSourceCompiles)

set(drone_sources
    devicemanager.cpp device.cpp emuldevice.cpp drone_main.cpp drone.cpp
    portmanager.cpp abstractport.cpp pcapport.cpp pcapsession.cpp
    pcaptransmitter.cpp pcaprxstats.cpp pcaptxstats.cpp pcaptxthread.cpp
    pcaptxttagstats.cpp bsdhostdevice.cpp bsdport.cpp linuxhostdevice.cpp linuxport.cpp
    linuxutils.cpp params.cpp streamtiming.cpp turbo.cpp winhostdevice.cpp
    winpcapport.cpp myservice.cpp pcapextra.cpp packetbuffer.cpp
    drone.h myservice.h pcaptransmitter.h streamtiming.h)

add_executable(drone ${drone_sources})
target_include_directories(drone PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(drone PRIVATE
    ostproto pbrpc Qt5::Core Qt5::Network Qt5::Xml protobuf::libprotobuf)

if(WIN32)
    set(NPCAP_ROOT "" CACHE PATH "Npcap SDK root directory")
    find_path(NPCAP_INCLUDE_DIR pcap.h
        HINTS "${NPCAP_ROOT}" "$ENV{NPCAP_ROOT}" PATH_SUFFIXES Include)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_npcap_library_suffix "Lib/x64")
    else()
        set(_npcap_library_suffix "Lib")
    endif()
    find_library(NPCAP_WPCAP_LIBRARY NAMES wpcap
        HINTS "${NPCAP_ROOT}" "$ENV{NPCAP_ROOT}"
        PATH_SUFFIXES "${_npcap_library_suffix}")
    find_library(NPCAP_PACKET_LIBRARY NAMES Packet packet
        HINTS "${NPCAP_ROOT}" "$ENV{NPCAP_ROOT}"
        PATH_SUFFIXES "${_npcap_library_suffix}")
    if(NOT NPCAP_INCLUDE_DIR OR NOT NPCAP_WPCAP_LIBRARY OR NOT NPCAP_PACKET_LIBRARY)
        message(FATAL_ERROR "Npcap SDK not found; set NPCAP_ROOT to its SDK directory")
    endif()
    target_include_directories(drone PRIVATE "${NPCAP_INCLUDE_DIR}")
    target_compile_definitions(drone PRIVATE
        WIN32_LEAN_AND_MEAN NTDDI_VERSION=0x06000000 _WIN32_WINNT=0x0600
        HAVE_REMOTE WPCAP)
    target_link_libraries(drone PRIVATE
        "${NPCAP_WPCAP_LIBRARY}" "${NPCAP_PACKET_LIBRARY}" iphlpapi)
    if(MINGW)
        target_link_options(drone PRIVATE -static)
    endif()
else()
    find_path(PCAP_INCLUDE_DIR pcap.h)
    find_library(PCAP_LIBRARY NAMES pcap)
    if(NOT PCAP_INCLUDE_DIR OR NOT PCAP_LIBRARY)
        message(FATAL_ERROR "libpcap development files not found")
    endif()
    target_include_directories(drone PRIVATE "${PCAP_INCLUDE_DIR}")
    target_link_libraries(drone PRIVATE "${PCAP_LIBRARY}" m)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBNL REQUIRED IMPORTED_TARGET libnl-3.0 libnl-route-3.0)
    target_link_libraries(drone PRIVATE PkgConfig::LIBNL)
    check_cxx_source_compiles("#include <linux/if_link.h>
        int main() { int value = IFLA_STATS64; return value == 0; }"
        HAVE_IFLA_STATS64)
    if(HAVE_IFLA_STATS64)
        target_compile_definitions(drone PRIVATE HAVE_IFLA_STATS64)
    endif()
endif()

ostinato_add_version_source(drone)
ostinato_target_defaults(drone)
ostinato_qt_target(drone)
if(APPLE)
    set_target_properties(drone PROPERTIES MACOSX_BUNDLE TRUE)
    install(TARGETS drone BUNDLE DESTINATION Ostinato)
else()
    install(TARGETS drone RUNTIME DESTINATION bin)
endif()
