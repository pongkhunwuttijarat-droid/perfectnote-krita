set(kritapdfio_SOURCES
    PdfIoPlugin.cpp
    PdfRendererSpike.cpp
    )

kis_add_library(kritapdfio MODULE ${kritapdfio_SOURCES})
target_link_libraries(kritapdfio PRIVATE kritaui)

if (ANDROID)
    # QJniObject / QJniEnvironment live in AndroidExtras on Qt5, and kritaui links that
    # module privately, so a plugin that talks to the framework has to ask for it itself.
    target_link_libraries(kritapdfio PRIVATE Qt${QT_MAJOR_VERSION}::AndroidExtras)
endif()

install(TARGETS kritapdfio DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
