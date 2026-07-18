set(client_sources
    arpstatusmodel.cpp clipboardhelper.cpp devicegroupdialog.cpp devicegroupmodel.cpp
    devicemodel.cpp deviceswidget.cpp dumpview.cpp stream.cpp hexlineedit.cpp
    logsmodel.cpp logswindow.cpp fieldedit.cpp findreplace.cpp main.cpp mainwindow.cpp
    mandatoryfieldsgroup.cpp ndpstatusmodel.cpp packetmodel.cpp params.cpp port.cpp
    portconfigdialog.cpp portgroup.cpp portgrouplist.cpp portmodel.cpp
    portstatsmodel.cpp portstatsfilterdialog.cpp portstatswindow.cpp portswindow.cpp
    portwidget.cpp preferences.cpp streamconfigdialog.cpp streamlistdelegate.cpp
    streammodel.cpp streamstatsmodel.cpp streamstatswindow.cpp streamswidget.cpp
    thememanager.cpp variablefieldswidget.cpp
    about.ui devicegroupdialog.ui deviceswidget.ui findreplace.ui logswindow.ui
    mainwindow.ui portconfigdialog.ui portstatsfilter.ui portstatswindow.ui
    portswindow.ui portwidget.ui preferences.ui streamconfigdialog.ui
    streamstatswindow.ui streamswidget.ui variablefieldswidget.ui
    ostinato.qrc
    arpstatusmodel.h clipboardhelper.h devicegroupdialog.h devicegroupmodel.h
    devicemodel.h deviceswidget.h fieldedit.h findreplace.h hexlineedit.h
    logsmodel.h logswindow.h mainwindow.h mandatoryfieldsgroup.h ndpstatusmodel.h
    port.h portgroup.h portgrouplist.h portmodel.h portstatsfilterdialog.h
    portstatsmodel.h portstatsproxymodel.h portstatswindow.h portswindow.h
    portwidget.h preferences.h streamconfigdialog.h streamlistdelegate.h
    streammodel.h streamstatsfiltermodel.h streamstatsmodel.h streamstatswindow.h
    streamswidget.h variablefieldswidget.h xtableview.h)

if(WIN32)
    list(APPEND client_sources ostinato.rc)
endif()
if(APPLE)
    list(APPEND client_sources icons/logo.icns)
    set_source_files_properties(icons/logo.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

add_executable(ostinato WIN32 MACOSX_BUNDLE ${client_sources})
if(APPLE)
    set_target_properties(ostinato PROPERTIES
        OUTPUT_NAME Ostinato MACOSX_BUNDLE_ICON_FILE logo.icns)
endif()
target_include_directories(ostinato PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ostinato PRIVATE
    ostfilegui ostfile ostprotogui ostproto pbrpc qhexedit2 protobuf::libprotobuf
    Qt5::Network Qt5::Script Qt5::Svg Qt5::Widgets Qt5::Xml)
if(TARGET modeltest)
    target_include_directories(ostinato PRIVATE
        "${PROJECT_SOURCE_DIR}/extra/modeltest")
    target_link_libraries(ostinato PRIVATE
        $<$<CONFIG:Debug>:modeltest> $<$<CONFIG:Debug>:Qt5::Test>)
endif()
if(MINGW)
    target_link_options(ostinato PRIVATE -static)
endif()

ostinato_add_version_source(ostinato)
ostinato_target_defaults(ostinato)

set(theme_files
    themes/material-dark.qss themes/material-dark.rcc
    themes/material-light.qss themes/material-light.rcc
    themes/qds-dark.qss themes/qds-dark.rcc
    themes/qds-light.qss themes/qds-light.rcc)
if(APPLE)
    install(TARGETS ostinato BUNDLE DESTINATION Ostinato)
    install(FILES ${theme_files}
        DESTINATION "Ostinato/Ostinato.app/Contents/SharedSupport/themes")
elseif(WIN32)
    install(TARGETS ostinato RUNTIME DESTINATION bin)
    install(FILES ${theme_files} DESTINATION bin/themes)
else()
    install(TARGETS ostinato RUNTIME DESTINATION bin)
    install(FILES ${theme_files} DESTINATION share/ostinato-controller/themes)
endif()
