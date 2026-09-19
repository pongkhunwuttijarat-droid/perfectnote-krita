# The backend runs the same code the plugin runs; the test compiles it directly so it can
# be exercised with ctest instead of having to start the whole application.
if (Poppler_Qt${QT_MAJOR_VERSION}_FOUND)
    add_executable(PdfRenderBackendTest
        PdfRenderBackendTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        )

    target_include_directories(PdfRenderBackendTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfRenderBackendTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION})

    target_compile_definitions(PdfRenderBackendTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    set_target_properties(PdfRenderBackendTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfRenderBackendTest COMMAND PdfRenderBackendTest)
    set_tests_properties(pdfio-PdfRenderBackendTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(PdfSessionTest
        PdfSessionTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        )

    target_include_directories(PdfSessionTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfSessionTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION})

    target_compile_definitions(PdfSessionTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    set_target_properties(PdfSessionTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfSessionTest COMMAND PdfSessionTest)
    set_tests_properties(pdfio-PdfSessionTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
