set(kritapdfio_SOURCES
    PdfIoDocker.cpp
    PdfIoPlugin.cpp
    PdfIoProbe.cpp
    PdfPageNavigator.cpp
    PdfRendererSpike.cpp
    backend/PdfRenderBackendCreate.cpp
    session/PdfExporter.cpp
    session/PdfPageSaver.cpp
    session/PdfProjectBuilder.cpp
    session/PdfSession.cpp
    session/PdfSessionManifest.cpp
    )

kis_add_library(kritapdfio MODULE ${kritapdfio_SOURCES})
target_link_libraries(kritapdfio PRIVATE kritaui)
target_include_directories(kritapdfio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The backend split: desktop reuses the Poppler dependency Krita already has, while Android
# has no Poppler at all and will reach android.graphics.pdf.PdfRenderer through QJniObject.
# Everything above PdfRenderBackend is platform neutral, which is the point of the split.
target_include_directories(kritapdfio PRIVATE ${CMAKE_SOURCE_DIR}/plugins/impex/libkra)
target_link_libraries(kritapdfio PRIVATE kritaimage kritalibkra kritawidgetutils)

if (Poppler_Qt${QT_MAJOR_VERSION}_FOUND)
    target_sources(kritapdfio PRIVATE backends/poppler/PopplerRenderBackend.cpp)
    target_link_libraries(kritapdfio PRIVATE Poppler::Qt${QT_MAJOR_VERSION})
    target_compile_definitions(kritapdfio PRIVATE PDFIO_HAVE_POPPLER)
endif()

if (ANDROID)
    # android.graphics.pdf.PdfRenderer is a framework class, so it is reached straight from C++
    # and no Java of ours has to be added to the APK.
    target_sources(kritapdfio PRIVATE
        AndroidDocumentPicker.cpp
        backends/android/AndroidRenderBackend.cpp)
endif()

if (ANDROID)
    # QJniObject / QJniEnvironment live in AndroidExtras on Qt5, and kritaui links that
    # module privately, so a plugin that talks to the framework has to ask for it itself.
    target_link_libraries(kritapdfio PRIVATE Qt${QT_MAJOR_VERSION}::AndroidExtras)
endif()

if (BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritapdfio DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install(FILES PdfIoPlugin.action DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions)
