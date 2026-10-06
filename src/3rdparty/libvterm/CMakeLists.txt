# Vendored libvterm (terminal emulation state machine), MIT license.
# Provenance and any local patches: NLANG-CHANGES.md. Static library so
# the nide deployment closure gains no new DLLs.
add_library(vterm_static STATIC
    src/encoding.c
    src/keyboard.c
    src/mouse.c
    src/parser.c
    src/pen.c
    src/screen.c
    src/state.c
    src/unicode.c
    src/vterm.c
)
target_include_directories(vterm_static
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(vterm_static PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
