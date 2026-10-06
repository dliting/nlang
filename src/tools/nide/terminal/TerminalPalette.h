/*---
TerminalPalette.h — the fixed 16-colour Campbell palette (Windows
Terminal's default dark scheme) injected into every VTerm instance, so
indexed colours always resolve the same way and the widget never does
palette lookups. RGB direct colours from SGR 38;2 travel alongside
untouched.
---*/
#pragma once
#include <vterm.h>

namespace nlang {
namespace terminal {

inline constexpr int kPaletteColorCount = 16;

//Campbell palette, indices 0-7 dim then 8-15 bright.
inline constexpr unsigned int kPaletteColors[kPaletteColorCount] = {
    0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00,
    0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
    0x767676, 0xE74856, 0x16C60C, 0xF9F1A5,
    0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2,
};

inline constexpr unsigned int kDefaultForeground = 0xCCCCCC;
inline constexpr unsigned int kDefaultBackground = 0x0C0C0C;

inline void SetTerminalColor(VTermColor* out, unsigned int rgb) {
    vterm_color_rgb(out,
                    (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

//Injects the palette into the state. Call after every reset as well —
//a hard reset restores libvterm's factory colours.
inline void ApplyTerminalPalette(VTerm* vterm) {
    VTermState* state = vterm_obtain_state(vterm);
    for (int i = 0; i < kPaletteColorCount; ++i) {
        VTermColor color;
        SetTerminalColor(&color, kPaletteColors[i]);
        vterm_state_set_palette_color(state, i, &color);
    }
}

inline void ApplyTerminalDefaultColors(VTermScreen* screen) {
    VTermColor foreground, background;
    SetTerminalColor(&foreground, kDefaultForeground);
    SetTerminalColor(&background, kDefaultBackground);
    vterm_screen_set_default_colors(screen, &foreground, &background);
}

} // namespace terminal
} // namespace nlang
