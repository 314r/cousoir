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
# and the tests. Qt Gui only for QImage, which covers need; Qt Xml and libzip
# to read the metadata of the books added.
set(COUSOIR_LIBRARY_SOURCES
    book.h
    book_edit.cpp
    book_import.cpp
    book_import.h
    book_operations.cpp
    book_operations.h
    book_edit.h
    calibre_config.cpp
    calibre_config.h
    calibre_date.h
    calibre_db.cpp
    calibre_db.h
    calibre_list.cpp
    calibre_list.h
    calibre_lock.cpp
    calibre_lock.h
    calibre_paths.cpp
    calibre_paths.h
    calibre_strings.inc
    calibre_text.cpp
    calibre_text.h
    calibre_unidecode.inc
    commit_engine.cpp
    commit_engine.h
    concurrency_guard.cpp
    concurrency_guard.h
    conversion.cpp
    conversion.h
    device_books.cpp
    device_books.h
    device_files.cpp
    device_files.h
    field_writers.cpp
    field_writers.h
    kobo.cpp
    kobo.h
    kindle.cpp
    kindle.h
    kindle_send.cpp
    kindle_send.h
    kobo_send.cpp
    kobo_send.h
    mtp.cpp
    mtp.h
    file_transaction.cpp
    file_transaction.h
    opf.cpp
    opf.h
    opf_reader.cpp
    opf_reader.h
    pdf_reader.cpp
    pdf_reader.h
    cousoir_paths.cpp
    cousoir_paths.h
    send_job.cpp
    send_job.h
    sqlite_db.cpp
    sqlite_db.h
)
set(COUSOIR_LIBRARY_LINKS Qt6::Core Qt6::Gui Qt6::Network Qt6::Xml libzip::zip ${COUSOIR_SQLITE} ${COUSOIR_MTP})

# The models QML uses on top of it.
set(COUSOIR_MODEL_SOURCES
    ${COUSOIR_LIBRARY_SOURCES}
    book_library.cpp
    book_library.h
    conversion_service.cpp
    conversion_service.h
    cover_cache.cpp
    cover_cache.h
    device_library.cpp
    device_library.h
)
set(COUSOIR_MODEL_LINKS ${COUSOIR_LIBRARY_LINKS} Qt6::Concurrent Qt6::Quick ${COUSOIR_DBUS})

qt_add_executable(cousoir
    main.cpp
    ${COUSOIR_MODEL_SOURCES}
    omarchy_theme.cpp
    omarchy_theme.h
    system_appearance.cpp
    system_appearance.h
)

set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

set(COUSOIR_QML_FILES
    AboutSettingsPage.qml
    ActionMenu.qml
    AppSession.qml
    AppearanceSettingsPage.qml
    BookDetailsPane.qml
    BookPage.qml
    BooksPage.qml
    ChoiceBox.qml
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
    SearchField.qml
    SettingsWindow.qml
    Sidebar.qml
    SidebarItem.qml
    StatTile.qml
    Theme.qml
    ThinScrollBar.qml
    TrashPage.qml
)

set(COUSOIR_ICON_FILES
    assets/icons/arrow-left.svg
    assets/icons/book.svg
    assets/icons/building.svg
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
        ${COUSOIR_MODEL_LINKS}
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Svg
)
target_compile_definitions(cousoir PRIVATE COUSOIR_VERSION="${COUSOIR_PACKAGE_VERSION}")

qt_add_executable(cousoir-cli
    cli_main.cpp
    ${COUSOIR_LIBRARY_SOURCES}
)
set_target_properties(cousoir-cli PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
target_link_libraries(cousoir-cli PRIVATE ${COUSOIR_LIBRARY_LINKS})
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
    endfunction()

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

    qt_add_executable(calibre_text_tests
        tests/calibre_text_test.cpp
        ${COUSOIR_LIBRARY_SOURCES}
    )
    target_link_libraries(calibre_text_tests PRIVATE ${COUSOIR_LIBRARY_LINKS} Qt6::Test)
    cousoir_configure_test_executable(calibre_text_tests)
    cousoir_add_logged_test(calibre_text_tests calibre_text_tests)

    qt_add_executable(calibre_paths_tests
        tests/calibre_paths_test.cpp
        ${COUSOIR_LIBRARY_SOURCES}
    )
    target_link_libraries(calibre_paths_tests PRIVATE ${COUSOIR_LIBRARY_LINKS} Qt6::Test)
    cousoir_configure_test_executable(calibre_paths_tests)
    cousoir_add_logged_test(calibre_paths_tests calibre_paths_tests)

    foreach(name IN ITEMS opf commit_engine conversion kobo kindle)
        qt_add_executable(${name}_tests
            tests/${name}_test.cpp
            ${COUSOIR_LIBRARY_SOURCES}
        )
        target_link_libraries(${name}_tests PRIVATE ${COUSOIR_LIBRARY_LINKS} Qt6::Test)
        cousoir_configure_test_executable(${name}_tests)
        cousoir_add_logged_test(${name}_tests ${name}_tests)
    endforeach()

    qt_add_executable(calibre_db_tests
        tests/calibre_db_test.cpp
        ${COUSOIR_LIBRARY_SOURCES}
    )
    target_link_libraries(calibre_db_tests PRIVATE ${COUSOIR_LIBRARY_LINKS} Qt6::Test)
    cousoir_configure_test_executable(calibre_db_tests)
    cousoir_add_logged_test(calibre_db_tests calibre_db_tests)

    qt_add_executable(concurrency_guard_tests
        tests/concurrency_guard_test.cpp
        ${COUSOIR_LIBRARY_SOURCES}
    )
    target_link_libraries(concurrency_guard_tests PRIVATE ${COUSOIR_LIBRARY_LINKS} Qt6::Test)
    cousoir_configure_test_executable(concurrency_guard_tests)
    cousoir_add_logged_test(concurrency_guard_tests concurrency_guard_tests)

    qt_add_executable(book_library_tests
        tests/book_library_test.cpp
        ${COUSOIR_MODEL_SOURCES}
    )
    target_link_libraries(book_library_tests PRIVATE ${COUSOIR_MODEL_LINKS} Qt6::Test)
    cousoir_configure_test_executable(book_library_tests)
    cousoir_add_logged_test(book_library_tests book_library_tests)
    set_tests_properties(book_library_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(library_ui_tests
        tests/library_ui_test.cpp
        ${COUSOIR_MODEL_SOURCES}
        omarchy_theme.cpp omarchy_theme.h
        system_appearance.cpp system_appearance.h
    )
    target_link_libraries(library_ui_tests PRIVATE
        ${COUSOIR_MODEL_LINKS} Qt6::Qml Qt6::QuickControls2 Qt6::QuickTest Qt6::Svg Qt6::Test
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

    # Compares cousoir-cli list with calibredb's own output on copies of the
    # libraries; skipped without Calibre.
    add_test(NAME golden_list
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/run.sh" "$<TARGET_FILE:cousoir-cli>"
    )
    set_tests_properties(golden_list PROPERTIES SKIP_RETURN_CODE 77)
endif()
