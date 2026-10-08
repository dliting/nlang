/*---
TerminalPalette.h — the fixed 16-colour VSCode Light Modern palette
(Windows Terminal's light scheme) injected into every VTerm instance, so
indexed colours always resolve the same way and the widget never does
palette lookups. RGB direct colours from SGR 38;2 travel alongside
untouched. The light theme keeps the terminal consistent with the rest of
nide's light windows: white background, black default text.
---*/
#pragma once
#include <vterm.h>

namespace nlang {
namespace terminal {

inline constexpr int kPaletteColorCount = 16;

//VSCode Light Modern palette, indices 0-7 dim then 8-15 bright.
inline constexpr unsigned int kPaletteColors[kPaletteColorCount] = {
    0x000000, 0xCD3131, 0x00BC00, 0x949800,
    0x0451A5, 0xBC05BC, 0x0598BC, 0x555555,
    0x666666, 0xCD3131, 0x14CE14, 0xB5BA00,
    0x0451A5, 0xBC05BC, 0x0598BC, 0xA5A5A5,
};

inline constexpr unsigned int kDefaultForeground = 0x000000;
inline constexpr unsigned int kDefaultBackground = 0xFFFFFF;

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
