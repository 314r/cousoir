cmake_minimum_required(VERSION 3.21)

project(cousoir VERSION 0.1.0 LANGUAGES CXX)

set(COUSOIR_APP_ID "io.github._314r.cousoir")
set(COUSOIR_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version used in package metadata")
if(NOT COUSOIR_PACKAGE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "COUSOIR_PACKAGE_VERSION must use MAJOR.MINOR.PATCH format")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CTest)

find_package(Qt6 6.9 REQUIRED COMPONENTS Core Concurrent Gui Network Qml Quick QuickControls2 Svg Xml)
find_package(SQLite3 REQUIRED)
find_package(libzip REQUIRED)
# UDisks2, to eject a Kobo.
if(UNIX AND NOT APPLE)
    find_package(Qt6 6.9 REQUIRED COMPONENTS DBus)
    set(COUSOIR_DBUS Qt6::DBus)
endif()
# libmtp, to reach a Kindle Paperwhite; without it, Cousoir sees no MTP device.
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
    book.h
    book_edit.cpp
    book_edit.h
    book_import.cpp
    book_import.h
    book_operations.cpp
    book_operations.h
    book_sort.cpp
    book_sort.h
    calibre_config.cpp
    calibre_config.h
    calibre_date.h
    calibre_db.cpp
    calibre_db.h
    calibre_job.cpp
    calibre_job.h
    calibre_languages.cpp
    calibre_languages.h
    calibre_list.cpp
    calibre_list.h
    calibre_lock.cpp
    calibre_lock.h
    calibre_paths.cpp
    calibre_paths.h
    calibre_search.cpp
    calibre_search.h
    calibre_search.inc
    calibre_strings.inc
    calibre_template.cpp
    calibre_template.h
    calibre_template_book.cpp
    calibre_template_engine.cpp
    calibre_template_functions.cpp
    calibre_template_p.h
    calibre_text.cpp
    calibre_text.h
    calibre_unidecode.inc
    commit_engine.cpp
    commit_engine.h
    concurrency_guard.cpp
    concurrency_guard.h
    conversion.cpp
    conversion.h
    cousoir_paths.cpp
    cousoir_paths.h
    description.cpp
    description.h
    device_books.cpp
    device_books.h
    device_files.cpp
    device_files.h
    field_writers.cpp
    field_writers.h
    file_transaction.cpp
    file_transaction.h
    identifier_links.cpp
    identifier_links.h
    kepub.cpp
    kepub.h
    kindle.cpp
    kindle.h
    kindle_send.cpp
    kindle_send.h
    kobo.cpp
    kobo.h
    kobo_send.cpp
    kobo_send.h
    library_check.cpp
    library_check.h
    metadata_download.cpp
    metadata_download.h
    mtp.cpp
    mtp.h
    opf.cpp
    opf.h
    opf_reader.cpp
    opf_reader.h
    palette.cpp
    palette.h
    pdf_reader.cpp
    pdf_reader.h
    python_json.cpp
    python_json.h
    reading_log.cpp
    reading_log.h
    send_job.cpp
    send_job.h
    sqlite_db.cpp
    sqlite_db.h
)
target_include_directories(cousoir_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Kobo's script, which a KEPUB carries.
qt_add_resources(cousoir_core "core_resources"
    PREFIX "/cousoir"
    BASE assets
    FILES assets/kobo/kobo.js
)
target_link_libraries(cousoir_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Xml libzip::zip ${COUSOIR_SQLITE} ${COUSOIR_MTP}
)

# The models QML uses on top of it, and the system's theme and appearance.
qt_add_library(cousoir_models STATIC
    book_library.cpp
    book_library.h
    book_list_model.cpp
    book_list_model.h
    conversion_service.cpp
    conversion_service.h
    cover_cache.cpp
    cover_cache.h
    device_library.cpp
    device_library.h
    metadata_downloader.cpp
    metadata_downloader.h
    omarchy_theme.cpp
    omarchy_theme.h
    system_appearance.cpp
    system_appearance.h
)
target_link_libraries(cousoir_models PUBLIC cousoir_core Qt6::Concurrent Qt6::Quick ${COUSOIR_DBUS})

qt_add_executable(cousoir main.cpp)

set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

set(COUSOIR_QML_FILES
    AboutSettingsPage.qml
    ActionMenu.qml
    AppSession.qml
    AppearanceSettingsPage.qml
    BookDetailsPane.qml
    BookPage.qml
    BooksPage.qml
    BulkEditPane.qml
    CheckPage.qml
    ChoiceBox.qml
    CommandPalette.qml
    ConfirmDialog.qml
    CoverGrid.qml
    DevicePage.qml
    Divider.qml
    FacetPage.qml
    FlatButton.qml
    FormField.qml
    HeadingLabel.qml
    LibraryFolderDialog.qml
    LibrarySettingsPage.qml
    ListTable.qml
    Main.qml
    MetaRow.qml
    MetadataDownloadDialog.qml
    SearchField.qml
    SettingsWindow.qml
    Sidebar.qml
    SidebarItem.qml
    Theme.qml
    ThemedWindow.qml
    ThinScrollBar.qml
    TrashPage.qml
)

set(COUSOIR_ICON_FILES
    assets/icons/arrow-left.svg
    assets/icons/book.svg
    assets/icons/building.svg
    assets/icons/check.svg
    assets/icons/chevron-down.svg
    assets/icons/close.svg
    assets/icons/eject.svg
    assets/icons/ereader.svg
    assets/icons/external.svg
    assets/icons/folder.svg
    assets/icons/globe.svg
    assets/icons/grid.svg
    assets/icons/image.svg
    assets/icons/info.svg
    assets/icons/layers.svg
    assets/icons/list.svg
    assets/icons/lock.svg
    assets/icons/panel-right.svg
    assets/icons/plus.svg
    assets/icons/refresh.svg
    assets/icons/search.svg
    assets/icons/settings-appearance.svg
    assets/icons/settings.svg
    assets/icons/star.svg
    assets/icons/tag.svg
    assets/icons/theme-dark.svg
    assets/icons/theme-light.svg
    assets/icons/theme-omarchy.svg
    assets/icons/theme-system.svg
    assets/icons/trash.svg
    assets/icons/user.svg
    assets/icons/hicolor/cousoir-256.png
)

qt_add_qml_module(cousoir
    URI Cousoir
    VERSION 1.0
    QML_FILES
        ${COUSOIR_QML_FILES}
    RESOURCES
        ${COUSOIR_ICON_FILES}
)

target_link_libraries(cousoir
    PRIVATE
        cousoir_models
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Svg
)
target_compile_definitions(cousoir PRIVATE COUSOIR_VERSION="${COUSOIR_PACKAGE_VERSION}")

qt_add_executable(cousoir-cli cli_main.cpp)
set_target_properties(cousoir-cli PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
target_link_libraries(cousoir-cli PRIVATE cousoir_core)
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
                          kindle library_check test_tool)
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
    set_tests_properties(book_library_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(library_ui_tests tests/library_ui_test.cpp)
    target_link_libraries(library_ui_tests PRIVATE
        cousoir_models Qt6::Qml Qt6::QuickControls2 Qt6::QuickTest Qt6::Svg Qt6::Test
    )
    cousoir_configure_test_executable(library_ui_tests)
    qt_add_resources(library_ui_tests "library_ui"
        PREFIX "/qt/qml/Cousoir"
        FILES
            ${COUSOIR_QML_FILES}
            ${COUSOIR_ICON_FILES}
    )
    foreach(scale IN ITEMS 1 2)
        cousoir_add_logged_test(library_ui_tests_scale_${scale} library_ui_tests)
        set_tests_properties(library_ui_tests_scale_${scale} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=${scale}"
        )
    endforeach()

    # Measures loading, searching, sorting and saving on a library, and the
    # window with --ui; run by hand on a large library, not by ctest.
    qt_add_executable(cousoir-benchmark tests/benchmark.cpp)
    target_link_libraries(cousoir-benchmark PRIVATE
        cousoir_models Qt6::Qml Qt6::QuickControls2 Qt6::QuickTest Qt6::Svg Qt6::Test
    )
    cousoir_configure_test_executable(cousoir-benchmark)
    qt_add_resources(cousoir-benchmark "benchmark_ui"
        PREFIX "/qt/qml/Cousoir"
        FILES
            ${COUSOIR_QML_FILES}
            ${COUSOIR_ICON_FILES}
    )

    # Compares cousoir-cli list with calibredb's own output on copies of the
    # libraries; skipped without Calibre.
    add_test(NAME golden_list
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/run.sh" "$<TARGET_FILE:cousoir-cli>"
            "$<TARGET_FILE:cousoir-test-tool>"
    )
    set_tests_properties(golden_list PROPERTIES SKIP_RETURN_CODE 77)
endif()
