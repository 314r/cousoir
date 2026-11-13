cmake_minimum_required(VERSION 3.21)

project(cousoir VERSION 0.1.0 LANGUAGES CXX)

set(COUSOIR_APP_ID "io.github._314r.cousoir")
set(COUSOIR_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version used in package metadata")
if(NOT COUSOIR_PACKAGE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "COUSOIR_PACKAGE_VERSION must use MAJOR.MINOR.PATCH format")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A build with the compiler's sanitizers, "address,undefined" for instance,
# for the tests to find what a crash would: make sanitize.
set(COUSOIR_SANITIZE "" CACHE STRING "Sanitizers to build with, as -fsanitize= takes them")
if(COUSOIR_SANITIZE)
    add_compile_options(-fsanitize=${COUSOIR_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=${COUSOIR_SANITIZE})
endif()

include(GNUInstallDirs)
include(CTest)

find_package(Qt6 6.9 REQUIRED COMPONENTS Core Concurrent Gui Network Qml Quick QuickControls2 Svg Xml)
find_package(SQLite3 REQUIRED)
# ICU and Snowball's libstemmer, for Calibre's full text tokenizer.
find_package(ICU REQUIRED COMPONENTS uc i18n)
find_path(STEMMER_INCLUDE_DIR libstemmer.h REQUIRED)
find_library(STEMMER_LIBRARY stemmer REQUIRED)
# xxHash, for the names Calibre gives the notes it keeps aside.
find_path(XXHASH_INCLUDE_DIR xxhash.h REQUIRED)
find_library(XXHASH_LIBRARY xxhash REQUIRED)
find_package(libzip REQUIRED)
# UDisks2, to eject a Kobo.
if(UNIX AND NOT APPLE)
    find_package(Qt6 6.9 REQUIRED COMPONENTS DBus)
    set(COUSOIR_DBUS Qt6::DBus)
endif()
# libmtp, to reach a Kindle Paperwhite; without it, Cousoir sees no MTP device.
# Qt WebEngine, to read books in Cousoir; without it, there is no reader.
# Qt TextToSpeech, to read them aloud.
find_package(Qt6 6.9 QUIET OPTIONAL_COMPONENTS WebEngineQuick PdfQuick ShaderTools TextToSpeech)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBMTP IMPORTED_TARGET libmtp)
endif()
if(LIBMTP_FOUND)
    set(COUSOIR_MTP PkgConfig::LIBMTP)
    add_compile_definitions(COUSOIR_HAVE_LIBMTP)
endif()
# CMake 4 renamed the imported target.
if(TARGET SQLite3::SQLite3)
    set(COUSOIR_SQLITE SQLite3::SQLite3)
else()
    set(COUSOIR_SQLITE SQLite::SQLite3)
endif()

qt_standard_project_setup(REQUIRES 6.9)

# Reading and writing the library, shared by the application, cousoir-cli
# and the tests, and built once for all of them. Qt Gui only for QImage,
# which covers need; Qt Xml and libzip to read the metadata of the books
# added.
qt_add_library(cousoir_core STATIC
    core/book.h
    core/book_edit.cpp
    core/book_edit.h
    core/book_import.cpp
    core/book_import.h
    core/book_operations.cpp
    core/book_operations.h
    core/book_sort.cpp
    core/book_sort.h
    core/calibre_config.cpp
    core/calibre_config.h
    core/calibre_date.h
    core/calibre_db.cpp
    core/calibre_fts.cpp
    core/calibre_fts.h
    core/calibre_notes.cpp
    core/calibre_notes.h
    core/calibre_db.h
    core/calibre_job.cpp
    core/calibre_job.h
    core/calibre_languages.cpp
    core/calibre_languages.h
    core/calibre_list.cpp
    core/calibre_list.h
    core/calibre_lock.cpp
    core/calibre_lock.h
    core/calibre_paths.cpp
    core/calibre_paths.h
    core/calibre_search.cpp
    core/calibre_search.h
    core/calibre_search.inc
    core/calibre_strings.inc
    core/calibre_template.cpp
    core/calibre_template.h
    core/calibre_template.inc
    core/calibre_template_book.cpp
    core/calibre_template_engine.cpp
    core/calibre_template_functions.cpp
    core/calibre_template_p.h
    core/calibre_text.cpp
    core/calibre_text.h
    core/calibre_unidecode.inc
    core/commit_engine.cpp
    core/commit_engine.h
    core/concurrency_guard.cpp
    core/concurrency_guard.h
    core/conversion.cpp
    core/conversion.h
    core/cousoir_paths.cpp
    core/cousoir_paths.h
    core/description.cpp
    core/description.h
    core/device_books.cpp
    core/device_books.h
    core/device_files.cpp
    core/device_files.h
    core/field_writers.cpp
    core/field_writers.h
    core/file_transaction.cpp
    core/file_transaction.h
    core/identifier_links.cpp
    core/identifier_links.h
    core/book_search.cpp
    core/book_search.h
    core/book_text_index.cpp
    core/book_text_index.h
    core/book_text_search.cpp
    core/book_text_search.h
    core/epub_book.cpp
    core/epub_book.h
    core/epub_package.cpp
    core/epub_package.h
    core/kepub.cpp
    core/kepub.h
    core/kindle.cpp
    core/kindle.h
    core/kindle_clippings.cpp
    core/kindle_clippings.h
    core/kindle_send.cpp
    core/kindle_send.h
    core/kf8_book.cpp
    core/kf8_book.h
    core/kf8_index.cpp
    core/kf8_index.h
    core/kf8_skeleton.cpp
    core/kf8_skeleton.h
    core/kf8_writer.cpp
    core/kf8_writer.h
    core/kobo.cpp
    core/kobo.h
    core/kobo_send.cpp
    core/kobo_send.h
    core/library_check.cpp
    core/library_check.h
    core/metadata_download.cpp
    core/metadata_download.h
    core/mobi_book.cpp
    core/new_library.cpp
    core/new_library.h
    core/save_to_disk.cpp
    core/save_to_disk.h
    core/text_book_epub.cpp
    core/text_book_epub.h
    core/text_book_metadata.cpp
    core/text_book_metadata.h
    core/mobi_book.h
    core/mobi_format.cpp
    core/mobi_format.h
    core/mobi_metadata.cpp
    core/mobi_metadata.h
    core/mtp.cpp
    core/mtp.h
    core/native_send.cpp
    core/native_send.h
    core/open_library.cpp
    core/open_library.h
    core/opf.cpp
    core/opf.h
    core/opf_reader.cpp
    core/opf_reader.h
    core/palette.cpp
    core/palette.h
    core/pdf_reader.cpp
    core/pdf_reader.h
    core/python_json.cpp
    core/python_json.h
    core/reading_log.cpp
    core/reading_log.h
    core/send_job.cpp
    core/send_job.h
    core/sqlite_db.cpp
    core/sqlite_db.h
    core/annotation_search.cpp
    core/annotation_search.h
    core/epub_cfi.cpp
    core/epub_cfi.h
    core/highlight_formats.cpp
    core/highlight_formats.h
    core/html_soup.cpp
    core/html_soup.h
    core/highlight_json.cpp
    core/highlight_json.h
    core/kobo_highlights.cpp
    core/kobo_highlights.h
    core/kobo_placement.cpp
    core/kobo_placement.h
    core/kobo_sync.cpp
    core/kobo_sync.h
    core/kobo_write.cpp
    core/kobo_write.h
    core/xml_tree.cpp
    core/xml_tree.h)
target_include_directories(cousoir_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
# Kobo's script, which a KEPUB carries.
qt_add_resources(cousoir_core "core_resources"
    PREFIX "/cousoir"
    BASE assets
    FILES assets/kobo/kobo.js assets/calibre/new_library.sql
)
target_link_libraries(cousoir_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Xml libzip::zip ${COUSOIR_SQLITE} ${COUSOIR_MTP}
    PRIVATE ICU::uc ICU::i18n ${STEMMER_LIBRARY} ${XXHASH_LIBRARY}
)
target_include_directories(cousoir_core PRIVATE ${STEMMER_INCLUDE_DIR} ${XXHASH_INCLUDE_DIR})

# The models QML uses on top of it, and the system's theme and appearance.
qt_add_library(cousoir_models STATIC
    models/app_instance.cpp
    models/app_instance.h
    models/book_library.cpp
    models/book_library.h
    models/book_library_commit.cpp
    models/book_library_edit.cpp
    models/book_library_import.cpp
    models/book_library_load.cpp
    models/book_library_reading.cpp
    models/book_library_support.h
    models/book_library_view.cpp
    models/book_list_model.cpp
    models/book_list_model.h
    models/book_text_indexer.cpp
    models/book_text_indexer.h
    models/bulk_metadata_downloader.cpp
    models/bulk_metadata_downloader.h
    models/conversion_service.cpp
    models/conversion_service.h
    models/cover_cache.cpp
    models/cover_cache.h
    models/device_library.cpp
    models/device_library.h
    models/highlight_browser.cpp
    models/highlight_browser.h
    models/highlight_document.cpp
    models/highlight_document.h
    models/highlight_sync.cpp
    models/highlight_sync.h
    models/metadata_downloader.cpp
    models/metadata_downloader.h
    models/omarchy_theme.cpp
    models/omarchy_theme.h
    models/portal_file_chooser.cpp
    models/portal_file_chooser.h
    models/system_appearance.cpp
    models/system_appearance.h
    models/translator.cpp
    models/translator.h)
# The interface's translations, one catalog per Omarchy language code.
qt_add_resources(cousoir_models "i18n"
    PREFIX "/"
    FILES
        i18n/en.json
        i18n/fr.json
)
target_include_directories(cousoir_models PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/models)
target_link_libraries(cousoir_models PUBLIC cousoir_core Qt6::Concurrent Qt6::Quick ${COUSOIR_DBUS})
if(TARGET Qt6::WebEngineQuick)
    target_sources(cousoir_models PRIVATE models/book_reader.cpp models/book_reader.h)
    target_link_libraries(cousoir_models PUBLIC Qt6::WebEngineQuick)
    target_compile_definitions(cousoir_models PUBLIC COUSOIR_READER)
    if(TARGET Qt6::TextToSpeech)
        target_sources(cousoir_models PRIVATE models/book_speech.cpp models/book_speech.h)
        target_link_libraries(cousoir_models PUBLIC Qt6::TextToSpeech)
        target_compile_definitions(cousoir_models PUBLIC COUSOIR_SPEECH)
    endif()
endif()

qt_add_executable(cousoir ui/main.cpp)

set_source_files_properties(ui/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

set(COUSOIR_QML_FILES
    ui/AboutSettingsPage.qml
    ui/ActionMenu.qml
    ui/AppSession.qml
    ui/AppShortcuts.qml
    ui/AppearanceSettingsPage.qml
    ui/BookDetailsPane.qml
    ui/BookPage.qml
    ui/BooksPage.qml
    ui/BulkDownloadDialog.qml
    ui/BulkEditPane.qml
    ui/CheckPage.qml
    ui/ChoiceBox.qml
    ui/CommandPalette.qml
    ui/ConfirmDialog.qml
    ui/CoverGrid.qml
    ui/DevicePage.qml
    ui/Divider.qml
    ui/FacetPage.qml
    ui/FilePicker.qml
    ui/FlatButton.qml
    ui/FormField.qml
    ui/HeadingLabel.qml
    ui/HighlightsPage.qml
    ui/LibraryFolderDialog.qml
    ui/LibrarySettingsPage.qml
    ui/ListRow.qml
    ui/ListTable.qml
    ui/Main.qml
    ui/MessagesPopup.qml
    ui/MainToolbar.qml
    ui/MetaRow.qml
    ui/MetadataDownloadDialog.qml
    ui/NotePanel.qml
    ui/RenameDialog.qml
    ui/SearchField.qml
    ui/SettingsWindow.qml
    ui/ShortcutsOverlay.qml
    ui/Sidebar.qml
    ui/SidebarItem.qml
    ui/Theme.qml
    ui/ReadingPage.qml
    ui/StatusBar.qml
    ui/TextSearchPage.qml
    ui/ThemedPopup.qml
    ui/ThemedWindow.qml
    ui/ThinScrollBar.qml
    ui/TrashPage.qml)
if(TARGET Qt6::WebEngineQuick)
    list(APPEND COUSOIR_QML_FILES ui/ReaderWindow.qml ui/ReaderGoToBox.qml ui/ReaderImageViewer.qml ui/ReaderNoteBox.qml
        ui/ReaderNoteEditor.qml ui/ReaderSettingsPopup.qml ui/ReaderToolbar.qml)
endif()
# Qt Pdf, to read PDFs in the reader; Qt Shader Tools to build the shader
# that shows its pages in the theme's colours.
if(TARGET Qt6::WebEngineQuick AND TARGET Qt6::PdfQuick)
    if(NOT TARGET Qt6::ShaderTools)
        message(FATAL_ERROR "The PDF reader needs Qt Shader Tools (qt6-shadertools) to build.")
    endif()
    list(APPEND COUSOIR_QML_FILES ui/PdfReaderWindow.qml)
endif()

# The QML in ui/, at the module's root in the resources as before: beside
# the assets and shaders their addresses name, "assets/icons/book.svg".
foreach(file IN LISTS COUSOIR_QML_FILES)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()

set(COUSOIR_ICON_FILES
    assets/icons/arrow-left.svg
    assets/icons/book.svg
    assets/icons/bookmark.svg
    assets/icons/bookmark-filled.svg
    assets/icons/building.svg
    assets/icons/check.svg
    assets/icons/chevron-down.svg
    assets/icons/close.svg
    assets/icons/copy.svg
    assets/icons/eject.svg
    assets/icons/ereader.svg
    assets/icons/external.svg
    assets/icons/folder.svg
    assets/icons/fullscreen.svg
    assets/icons/globe.svg
    assets/icons/highlighter.svg
    assets/icons/grid.svg
    assets/icons/image.svg
    assets/icons/info.svg
    assets/icons/layers.svg
    assets/icons/list.svg
    assets/icons/lock.svg
    assets/icons/markdown.svg
    assets/icons/more.svg
    assets/icons/note.svg
    assets/icons/panel-right.svg
    assets/icons/pause.svg
    assets/icons/play.svg
    assets/icons/plus.svg
    assets/icons/refresh.svg
    assets/icons/search.svg
    assets/icons/settings-appearance.svg
    assets/icons/settings.svg
    assets/icons/skip-back.svg
    assets/icons/skip-forward.svg
    assets/icons/speaker.svg
    assets/icons/stop.svg
    assets/icons/tag.svg
    assets/icons/theme-dark.svg
    assets/icons/theme-light.svg
    assets/icons/theme-omarchy.svg
    assets/icons/theme-system.svg
    assets/icons/trash.svg
    assets/icons/type.svg
    assets/icons/user.svg
    assets/icons/hicolor/cousoir-256.png
    assets/reader/reader.js
)

# The interface, its QML compiled ahead of time, in a library of its own: the
# UI tests and the benchmark run the very code Cousoir runs, which can differ
# from the QML read as it is written (see DOC.md, "QML compiled ahead of
# time").
qt_add_library(cousoir_ui STATIC)
qt_add_qml_module(cousoir_ui
    URI Cousoir
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Cousoir
    QML_FILES
        ${COUSOIR_QML_FILES}
    RESOURCES
        ${COUSOIR_ICON_FILES}
)
if(TARGET Qt6::WebEngineQuick AND TARGET Qt6::PdfQuick)
    qt_add_shaders(cousoir_ui "cousoir_shaders"
        PREFIX "/qt/qml/Cousoir"
        FILES shaders/themed_pages.frag
    )
endif()
target_link_libraries(cousoir_ui
    PRIVATE
        cousoir_models
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Svg
)

target_link_libraries(cousoir
    PRIVATE
        cousoir_models
        cousoir_uiplugin
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Svg
)
target_compile_definitions(cousoir PRIVATE COUSOIR_VERSION="${COUSOIR_PACKAGE_VERSION}")

qt_add_executable(cousoir-cli cli/cli_main.cpp cli/cli_apply.cpp cli/cli_commands.h cli/cli_common.cpp cli/cli_devices.cpp
    cli/cli_library.cpp)
set_target_properties(cousoir-cli PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
target_link_libraries(cousoir-cli PRIVATE cousoir_core)
# The skill `cousoir-cli skill install` gives coding agents.
qt_add_resources(cousoir-cli "cli_resources"
    PREFIX "/cousoir"
    BASE assets
    FILES assets/skill.md
)
target_compile_definitions(cousoir-cli PRIVATE COUSOIR_VERSION="${COUSOIR_PACKAGE_VERSION}")

install(TARGETS cousoir cousoir-cli
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(UNIX AND NOT APPLE)
    install(FILES "${COUSOIR_APP_ID}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )
    install(FILES "${COUSOIR_APP_ID}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
        RENAME "${COUSOIR_APP_ID}.appdata.xml"
    )
    foreach(icon_size 16 32 48 64 128 256 512)
        install(FILES "assets/icons/hicolor/cousoir-${icon_size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps"
            RENAME "${COUSOIR_APP_ID}.png"
        )
    endforeach()
    install(FILES assets/icons/cousoir.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME "${COUSOIR_APP_ID}.svg"
    )
    install(FILES LICENSE
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cousoir"
        RENAME copyright
    )
    install(FILES README.md DOC.md CREDITS.md
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cousoir"
    )
    install(DIRECTORY LICENSES
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/cousoir"
    )
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.9 REQUIRED COMPONENTS QuickTest Test)

    # qt_add_executable defers qt_finalize_target, and that finalizer turns the
    # binary back into a Windows GUI app. Stdout is then detached from ctest.
    # Re-apply the console subsystem only after the target is finalized.
    function(cousoir_force_windows_console target)
        if(NOT WIN32 OR NOT TARGET "${target}")
            return()
        endif()
        get_target_property(is_finalized "${target}" _qt_is_finalized)
        if(NOT is_finalized)
            cmake_language(DEFER CALL cousoir_force_windows_console "${target}")
            return()
        endif()
        set_target_properties("${target}" PROPERTIES WIN32_EXECUTABLE FALSE)
        if(MSVC)
            target_link_options("${target}" PRIVATE "/SUBSYSTEM:CONSOLE")
        endif()
    endfunction()

    function(cousoir_configure_test_executable target)
        if(WIN32)
            cmake_language(DEFER CALL cousoir_force_windows_console "${target}")
        endif()
        target_include_directories("${target}" PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions("${target}" PRIVATE
            COUSOIR_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
            COUSOIR_VERSION="${COUSOIR_PACKAGE_VERSION}")
        if(NOT target STREQUAL "cousoir-test-tool")
            target_compile_definitions("${target}" PRIVATE
                COUSOIR_TEST_TOOL="$<TARGET_FILE:cousoir-test-tool>")
            add_dependencies("${target}" cousoir-test-tool)
        endif()
    endfunction()

    # What the tests run besides Cousoir and Calibre: the golden tests'
    # comparisons and fake readers, the fixtures' book files and the fake
    # sending job.
    qt_add_executable(cousoir-test-tool
        tests/tool/common.cpp
        tests/tool/common.h
        tests/tool/compare.cpp
        tests/tool/fixtures.cpp
        tests/tool/large.cpp
        tests/tool/library.cpp
        tests/tool/main.cpp
        tests/tool/readers.cpp
        tests/tool/search.cpp
        tests/tool/sort.cpp
    )
    target_link_libraries(cousoir-test-tool PRIVATE cousoir_core)
    cousoir_configure_test_executable(cousoir-test-tool)

    set(COUSOIR_TEST_LOG_DIR "${CMAKE_BINARY_DIR}/test-results")
    file(MAKE_DIRECTORY "${COUSOIR_TEST_LOG_DIR}")

    # -o -,txt keeps the existing ctest stream. The file copy survives a GUI
    # subsystem, and CI prints it when a test fails.
    function(cousoir_add_logged_test name target)
        add_test(
            NAME "${name}"
            COMMAND "${target}"
                -o -,txt
                -o "${COUSOIR_TEST_LOG_DIR}/${name}.txt,txt"
        )
    endfunction()

    foreach(name IN ITEMS calibre_text calibre_paths calibre_search calibre_db description opf commit_engine concurrency_guard conversion kobo
                          kindle kf8 library_check metadata_web mobi_metadata epub_book epub_cfi calibre_fts calibre_notes annotation_search
                          kobo_highlights kobo_placement kobo_write test_tool damaged_books)
        qt_add_executable(${name}_tests tests/${name}_test.cpp)
        target_link_libraries(${name}_tests PRIVATE cousoir_core Qt6::Test)
        cousoir_configure_test_executable(${name}_tests)
        cousoir_add_logged_test(${name}_tests ${name}_tests)
    endforeach()

    # A description is read with QTextDocument, which wants a GUI application.
    set_tests_properties(description_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(book_library_tests tests/book_library_test.cpp)
    target_link_libraries(book_library_tests PRIVATE cousoir_models Qt6::Test)
    cousoir_configure_test_executable(book_library_tests)
    cousoir_add_logged_test(book_library_tests book_library_tests)
    # Each save that reads the library again in part checks it against the
    # library read whole.
    set_tests_properties(book_library_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;COUSOIR_VERIFY_COMMITS=1")

    # The language and the window rounding Cousoir follows on Omarchy.
    qt_add_executable(omarchy_tests tests/omarchy_test.cpp)
    target_link_libraries(omarchy_tests PRIVATE cousoir_models Qt6::Test)
    cousoir_configure_test_executable(omarchy_tests)
    cousoir_add_logged_test(omarchy_tests omarchy_tests)
    set_tests_properties(omarchy_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=")

    qt_add_executable(library_ui_tests tests/library_ui_test.cpp)
    target_link_libraries(library_ui_tests PRIVATE
        cousoir_models cousoir_uiplugin Qt6::Qml Qt6::QuickControls2 Qt6::QuickTest Qt6::Svg Qt6::Test
    )
    cousoir_configure_test_executable(library_ui_tests)
    foreach(scale IN ITEMS 1 2)
        cousoir_add_logged_test(library_ui_tests_scale_${scale} library_ui_tests)
        set_tests_properties(library_ui_tests_scale_${scale} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=${scale};QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu;COUSOIR_VERIFY_COMMITS=1"
        )
    endforeach()

    # Measures loading, searching, sorting and saving on a library, and the
    # window with --ui; run by hand on a large library, not by ctest.
    qt_add_executable(cousoir-benchmark tests/benchmark.cpp)
    target_link_libraries(cousoir-benchmark PRIVATE
        cousoir_models cousoir_uiplugin Qt6::Qml Qt6::QuickControls2 Qt6::QuickTest Qt6::Svg Qt6::Test
    )
    cousoir_configure_test_executable(cousoir-benchmark)
    # Cousoir's times and memory on 50,000 books against tests/bench/budgets.json,
    # only when asked: COUSOIR_BENCH=1 ctest -L bench.
    add_test(NAME bench_50k COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tools/bench_budget.sh" "${CMAKE_BINARY_DIR}")
    set_tests_properties(bench_50k PROPERTIES LABELS bench SKIP_RETURN_CODE 77 TIMEOUT 900)

    # Compares cousoir-cli list with calibredb's own output on copies of the
    # libraries; skipped without Calibre.
    add_test(NAME golden_list
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/run.sh" "$<TARGET_FILE:cousoir-cli>"
            "$<TARGET_FILE:cousoir-test-tool>"
    )
    set_tests_properties(golden_list PROPERTIES SKIP_RETURN_CODE 77)

    # The interface's compiled QML looked through for a value returned that
    # no code set, as Qt 6.12 compiles `a ? { … } : { … }`.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME compiled_qml
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_compiled_qml.py"
                "${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmlcache" cousoir_ui_
        )
        # English has the plural forms of every counted string; the test
        # also lists what the other catalogs lack. Skipped without lupdate.
        add_test(NAME i18n_catalogs
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/i18n.py"
        )
        set_tests_properties(i18n_catalogs PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
