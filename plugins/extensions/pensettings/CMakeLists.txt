set(kritapensettings_SOURCES
    PenSettings.cpp
    KisPenSettings.cpp
    )

ki18n_wrap_ui(kritapensettings_SOURCES
    wdg_pensettings.ui
    )

kis_add_library(kritapensettings MODULE ${kritapensettings_SOURCES})
target_link_libraries(kritapensettings kritaui)
install(TARGETS kritapensettings DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

install( FILES  PenSettings.action  DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions)
