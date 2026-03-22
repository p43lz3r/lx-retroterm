find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

add_executable(ttcore-gui
    main.c
    app_window.c
    config.c
    menu.c
    toolbar.c
    toolbar_toggle.c
    view_settings.c
    terminal_view.c
    status_bar.c
    serial_conn.c
    serial_config_dialog.c
    text_sender.c
    hex_upload.c
    xmodem_gui.c
    session_log.c
)

target_include_directories(ttcore-gui PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GTK3_INCLUDE_DIRS}
)

target_compile_options(ttcore-gui PRIVATE
    ${GTK3_CFLAGS_OTHER}
)

target_link_libraries(ttcore-gui
    ttcore
    ${GTK3_LIBRARIES}
)
