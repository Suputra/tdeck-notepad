#pragma once

#include <stdint.h>

// Precomputed low-fi glyphs for the power-off ASCII art.
// Each glyph is a 3x4 bitmap (bit 2 is left-most pixel).
// `shade` controls how densely lit pixels are rendered to mimic grayscale.
struct PoweroffGlyph {
    char ch;
    uint8_t rows[4];
    uint8_t shade; // 0: sparse, 1: light, 2: medium, 3: solid
};

static constexpr int POWEROFF_ART_CELL_W = 3;
static constexpr int POWEROFF_ART_CELL_H = 4;

static constexpr PoweroffGlyph kPoweroffGlyphs[] = {
    { ' ', { 0b000, 0b000, 0b000, 0b000 }, 0 },
    { '\'', { 0b010, 0b010, 0b000, 0b000 }, 0 },
    { ',', { 0b000, 0b000, 0b010, 0b100 }, 0 },
    { '.', { 0b000, 0b000, 0b000, 0b010 }, 0 },
    { '0', { 0b111, 0b101, 0b101, 0b111 }, 3 },
    { ':', { 0b000, 0b010, 0b000, 0b010 }, 0 },
    { ';', { 0b010, 0b000, 0b010, 0b100 }, 1 },
    { 'K', { 0b101, 0b110, 0b110, 0b101 }, 3 },
    { 'N', { 0b101, 0b111, 0b111, 0b101 }, 3 },
    { 'O', { 0b111, 0b101, 0b101, 0b111 }, 3 },
    { 'W', { 0b101, 0b111, 0b111, 0b101 }, 3 },
    { 'X', { 0b101, 0b010, 0b010, 0b101 }, 3 },
    { 'c', { 0b011, 0b100, 0b100, 0b011 }, 1 },
    { 'd', { 0b011, 0b101, 0b101, 0b111 }, 2 },
    { 'k', { 0b100, 0b101, 0b110, 0b101 }, 2 },
    { 'l', { 0b010, 0b010, 0b010, 0b010 }, 1 },
    { 'o', { 0b010, 0b101, 0b101, 0b010 }, 2 },
    { 'x', { 0b101, 0b010, 0b010, 0b101 }, 2 },
};

inline const PoweroffGlyph& poweroffGlyphFor(char ch) {
    for (const auto& glyph : kPoweroffGlyphs) {
        if (glyph.ch == ch) return glyph;
    }
    return kPoweroffGlyphs[0];
}

inline bool poweroffShadeAllowsPixel(uint8_t shade, int px, int py) {
    switch (shade) {
        case 0: return ((px + py) % 4) == 0;
        case 1: return ((px + py) % 3) == 0;
        case 2: return ((px + py) & 1) == 0;
        default: return true;
    }
}
