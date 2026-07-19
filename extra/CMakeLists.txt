file(GLOB qhexedit_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/qhexedit2/src/*.h")
add_library(qhexedit2 STATIC
    qhexedit2/src/chunks.cpp qhexedit2/src/commands.cpp qhexedit2/src/qhexedit.cpp
    ${qhexedit_headers})
target_compile_definitions(qhexedit2 PRIVATE QHEXEDIT_EXPORTS)
target_include_directories(qhexedit2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/qhexedit2/src")
target_link_libraries(qhexedit2 PUBLIC Qt5::Core Qt5::Gui Qt5::Widgets)
ostinato_qt_target(qhexedit2)

if(Qt5Core_VERSION VERSION_GREATER_EQUAL 5.7)
    add_library(modeltest STATIC
        modeltest/modeltest.cpp modeltest/dynamictreemodel.cpp
        modeltest/modeltest.h modeltest/dynamictreemodel.h)
    target_include_directories(modeltest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/modeltest")
    target_link_libraries(modeltest PUBLIC Qt5::Test Qt5::Widgets)
    ostinato_qt_target(modeltest)
endif()
