#include "display_font.h"

int display_utf8_decode(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p[0] == 0) {
        *cp = 0;
        return 0;
    }
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }

    int len;
    uint32_t value;
    if ((p[0] & 0xE0) == 0xC0) {
        len = 2;
        value = p[0] & 0x1F;
    } else if ((p[0] & 0xF0) == 0xE0) {
        len = 3;
        value = p[0] & 0x0F;
    } else if ((p[0] & 0xF8) == 0xF0) {
        len = 4;
        value = p[0] & 0x07;
    } else {
        *cp = 0xFFFD;
        return 1;
    }

    for (int i = 1; i < len; i++) {
        if (p[i] == 0 || (p[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return i;
        }
        value = (value << 6) | (p[i] & 0x3F);
    }

    *cp = value;
    return len;
}

const uint8_t *display_font_lookup(uint32_t cp, int *width, int *height)
{
    if (cp >= FONT_ASCII_FIRST && cp <= FONT_ASCII_LAST) {
        *width = 8;
        *height = 16;
        return font_ascii[cp - FONT_ASCII_FIRST];
    }

    *width = 16;
    *height = 16;

    size_t lo = 0;
    size_t hi = font_cjk_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (font_cjk[mid].unicode < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo < font_cjk_count && font_cjk[lo].unicode == cp) {
        return font_cjk[lo].glyph;
    }
    return NULL;
}
