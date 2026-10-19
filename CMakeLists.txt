cmake_minimum_required(VERSION 3.21)

project(reliure VERSION 0.1.0 LANGUAGES CXX)

set(RELIURE_APP_ID "io.github._314r.reliure")
set(RELIURE_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version used in package metadata")
if(NOT RELIURE_PACKAGE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "RELIURE_PACKAGE_VERSION must use MAJOR.MINOR.PATCH format")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CTest)

find_package(Qt6 6.9 REQUIRED COMPONENTS Core Concurrent Gui Network Qml Quick QuickControls2 Svg Xml)
find_package(SQLite3 REQUIRED)
find_package(libzip REQUIRED)
# CMake 4 renamed the imported target.
if(TARGET SQLite3::SQLite3)
    set(RELIURE_SQLITE SQLite3::SQLite3)
else()
    set(RELIURE_SQLITE SQLite::SQLite3)
endif()

qt_standard_project_setup(REQUIRES 6.9)

# Reading and writing the library, shared by the application, reliure-cli
# and the tests. Qt Gui only for QImage, which covers need; Qt Xml and libzip
# to read the metadata of the books added.
set(RELIURE_LIBRARY_SOURCES
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
    field_writers.cpp
    field_writers.h
    file_transaction.cpp
    file_transaction.h
    opf.cpp
    opf.h
    opf_reader.cpp
    opf_reader.h
    pdf_reader.cpp
    pdf_reader.h
    reliure_paths.cpp
    reliure_paths.h
    sqlite_db.cpp
    sqlite_db.h
)
set(RELIURE_LIBRARY_LINKS Qt6::Core Qt6::Gui Qt6::Network Qt6::Xml libzip::zip ${RELIURE_SQLITE})

# The models QML uses on top of it.
set(RELIURE_MODEL_SOURCES
    ${RELIURE_LIBRARY_SOURCES}
    book_library.cpp
    book_library.h
    cover_cache.cpp
    cover_cache.h
)
set(RELIURE_MODEL_LINKS ${RELIURE_LIBRARY_LINKS} Qt6::Concurrent Qt6::Quick)

qt_add_executable(reliure
    main.cpp
    ${RELIURE_MODEL_SOURCES}
    omarchy_theme.cpp
    omarchy_theme.h
    system_appearance.cpp
    system_appearance.h
)

set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

set(RELIURE_QML_FILES
    ActionMenu.qml
    AppSession.qml
    AppearanceSettingsPage.qml
    BookDetailsPane.qml
    BookPage.qml
    BooksPage.qml
    ChoiceBox.qml
    ConfirmDialog.qml
    CoverGrid.qml
    Divider.qml
    FacetPage.qml
    FlatButton.qml
    FormField.qml
    HeadingLabel.qml
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

set(RELIURE_ICON_FILES
    assets/icons/arrow-left.svg
    assets/icons/book.svg
    assets/icons/building.svg
    assets/icons/chevron-down.svg
    assets/icons/close.svg
    assets/icons/external.svg
    assets/icons/folder.svg
    assets/icons/globe.svg
    assets/icons/grid.svg
    assets/icons/image.svg
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
    assets/icons/hicolor/reliure-256.png
)

qt_add_qml_module(reliure
    URI Reliure
    VERSION 1.0
    QML_FILES
        ${RELIURE_QML_FILES}
    RESOURCES
        ${RELIURE_ICON_FILES}
)

target_link_libraries(reliure
    PRIVATE
        ${RELIURE_MODEL_LINKS}
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Svg
)
target_compile_definitions(reliure PRIVATE RELIURE_VERSION="${RELIURE_PACKAGE_VERSION}")

qt_add_executable(reliure-cli
    cli_main.cpp
    ${RELIURE_LIBRARY_SOURCES}
)
set_target_properties(reliure-cli PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
target_link_libraries(reliure-cli PRIVATE ${RELIURE_LIBRARY_LINKS})
target_compile_definitions(reliure-cli PRIVATE RELIURE_VERSION="${RELIURE_PACKAGE_VERSION}")

install(TARGETS reliure reliure-cli
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(UNIX AND NOT APPLE)
    install(FILES "${RELIURE_APP_ID}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )
    install(FILES "${RELIURE_APP_ID}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
        RENAME "${RELIURE_APP_ID}.appdata.xml"
    )
    foreach(icon_size 16 32 48 64 128 256 512)
        install(FILES "assets/icons/hicolor/reliure-${icon_size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps"
            RENAME "${RELIURE_APP_ID}.png"
        )
    endforeach()
    install(FILES LICENSE
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/reliure"
        RENAME copyright
    )
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.9 REQUIRED COMPONENTS QuickTest Test)

    # qt_add_executable defers qt_finalize_target, and that finalizer turns the
    # binary back into a Windows GUI app. Stdout is then detached from ctest.
    # Re-apply the console subsystem only after the target is finalized.
    function(reliure_force_windows_console target)
        if(NOT WIN32 OR NOT TARGET "${target}")
            return()
        endif()
        get_target_property(is_finalized "${target}" _qt_is_finalized)
        if(NOT is_finalized)
            cmake_language(DEFER CALL reliure_force_windows_console "${target}")
            return()
        endif()
        set_target_properties("${target}" PROPERTIES WIN32_EXECUTABLE FALSE)
        if(MSVC)
            target_link_options("${target}" PRIVATE "/SUBSYSTEM:CONSOLE")
        endif()
    endfunction()

    function(reliure_configure_test_executable target)
        if(WIN32)
            cmake_language(DEFER CALL reliure_force_windows_console "${target}")
        endif()
        target_include_directories("${target}" PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions("${target}" PRIVATE
            RELIURE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
            RELIURE_VERSION="${RELIURE_PACKAGE_VERSION}")
    endfunction()

    set(RELIURE_TEST_LOG_DIR "${CMAKE_BINARY_DIR}/test-results")
    file(MAKE_DIRECTORY "${RELIURE_TEST_LOG_DIR}")

    # -o -,txt keeps the existing ctest stream. The file copy survives a GUI
    # subsystem, and CI prints it when a test fails.
    function(reliure_add_logged_test name target)
        add_test(
            NAME "${name}"
            COMMAND "${target}"
                -o -,txt
                -o "${RELIURE_TEST_LOG_DIR}/${name}.txt,txt"
        )
    endfunction()

    qt_add_executable(calibre_text_tests
        tests/calibre_text_test.cpp
        ${RELIURE_LIBRARY_SOURCES}
    )
    target_link_libraries(calibre_text_tests PRIVATE ${RELIURE_LIBRARY_LINKS} Qt6::Test)
    reliure_configure_test_executable(calibre_text_tests)
    reliure_add_logged_test(calibre_text_tests calibre_text_tests)

    qt_add_executable(calibre_paths_tests
        tests/calibre_paths_test.cpp
        ${RELIURE_LIBRARY_SOURCES}
    )
    target_link_libraries(calibre_paths_tests PRIVATE ${RELIURE_LIBRARY_LINKS} Qt6::Test)
    reliure_configure_test_executable(calibre_paths_tests)
    reliure_add_logged_test(calibre_paths_tests calibre_paths_tests)

    foreach(name IN ITEMS opf commit_engine)
        qt_add_executable(${name}_tests
            tests/${name}_test.cpp
            ${RELIURE_LIBRARY_SOURCES}
        )
        target_link_libraries(${name}_tests PRIVATE ${RELIURE_LIBRARY_LINKS} Qt6::Test)
        reliure_configure_test_executable(${name}_tests)
        reliure_add_logged_test(${name}_tests ${name}_tests)
    endforeach()

    qt_add_executable(calibre_db_tests
        tests/calibre_db_test.cpp
        ${RELIURE_LIBRARY_SOURCES}
    )
    target_link_libraries(calibre_db_tests PRIVATE ${RELIURE_LIBRARY_LINKS} Qt6::Test)
    reliure_configure_test_executable(calibre_db_tests)
    reliure_add_logged_test(calibre_db_tests calibre_db_tests)

    qt_add_executable(concurrency_guard_tests
        tests/concurrency_guard_test.cpp
        ${RELIURE_LIBRARY_SOURCES}
    )
    target_link_libraries(concurrency_guard_tests PRIVATE ${RELIURE_LIBRARY_LINKS} Qt6::Test)
    reliure_configure_test_executable(concurrency_guard_tests)
    reliure_add_logged_test(concurrency_guard_tests concurrency_guard_tests)

    qt_add_executable(book_library_tests
        tests/book_library_test.cpp
        ${RELIURE_MODEL_SOURCES}
    )
    target_link_libraries(book_library_tests PRIVATE ${RELIURE_MODEL_LINKS} Qt6::Test)
    reliure_configure_test_executable(book_library_tests)
    reliure_add_logged_test(book_library_tests book_library_tests)
    set_tests_properties(book_library_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(library_ui_tests
        tests/library_ui_test.cpp
        ${RELIURE_MODEL_SOURCES}
        omarchy_theme.cpp omarchy_theme.h
        system_appearance.cpp system_appearance.h
    )
    target_link_libraries(library_ui_tests PRIVATE
        ${RELIURE_MODEL_LINKS} Qt6::Qml Qt6::QuickControls2 Qt6::QuickTest Qt6::Svg Qt6::Test
    )
    reliure_configure_test_executable(library_ui_tests)
    qt_add_resources(library_ui_tests "library_ui"
        PREFIX "/qt/qml/Reliure"
        FILES
            ${RELIURE_QML_FILES}
            ${RELIURE_ICON_FILES}
    )
    foreach(scale IN ITEMS 1 2)
        reliure_add_logged_test(library_ui_tests_scale_${scale} library_ui_tests)
        set_tests_properties(library_ui_tests_scale_${scale} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=${scale}"
        )
    endforeach()

    # Compares reliure-cli list with calibredb's own output on copies of the
    # libraries; skipped without Calibre.
    add_test(NAME golden_list
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/run.sh" "$<TARGET_FILE:reliure-cli>"
    )
    set_tests_properties(golden_list PROPERTIES SKIP_RETURN_CODE 77)
endif()
