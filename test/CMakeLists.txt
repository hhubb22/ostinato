if(BUILD_TESTING)
    add_executable(ostinato_compatibility_tests compatibility_tests.cpp)
    target_include_directories(ostinato_compatibility_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/common" "${PROJECT_SOURCE_DIR}/rpc")
    target_link_libraries(ostinato_compatibility_tests PRIVATE
        ostfile ostproto pbrpc Qt5::Test)
    ostinato_target_defaults(ostinato_compatibility_tests)

    add_test(NAME compatibility COMMAND ostinato_compatibility_tests)
endif()

# Legacy command-line PCAP import smoke tool from test/test.pro. It remains a
# manually built target and is intentionally not registered with CTest.
add_executable(ostinato_importpcap EXCLUDE_FROM_ALL main.cpp)
target_include_directories(ostinato_importpcap PRIVATE
    "${PROJECT_SOURCE_DIR}/common" "${PROJECT_SOURCE_DIR}/client")
target_link_libraries(ostinato_importpcap PRIVATE ostfile ostproto Qt5::Core)
if(WIN32)
    target_link_libraries(ostinato_importpcap PRIVATE wpcap Packet)
else()
    find_library(TEST_PCAP_LIBRARY NAMES pcap REQUIRED)
    target_link_libraries(ostinato_importpcap PRIVATE "${TEST_PCAP_LIBRARY}")
endif()
ostinato_target_defaults(ostinato_importpcap)
