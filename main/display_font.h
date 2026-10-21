#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_ASCII_FIRST 0x20
#define FONT_ASCII_LAST 0x7E
#define FONT_ASCII_COUNT (FONT_ASCII_LAST - FONT_ASCII_FIRST + 1)

typedef struct {
    uint16_t unicode;
    uint8_t glyph[32]; /* 16 行，每行 2 字节，MSB 在左 */
} font_glyph_t;

/* 8x16 ASCII：每行 1 字节 */
extern const uint8_t font_ascii[FONT_ASCII_COUNT][16];
/* 16x16 GB2312 标点与一级汉字，按 Unicode 升序排列 */
extern const font_glyph_t font_cjk[];
extern const size_t font_cjk_count;

/**
 * @brief 解码一个 UTF-8 字符
 *
 * 非法序列返回 1 且 *cp = 0xFFFD；字符串结束时返回 0 且 *cp = 0。
 *
 * @return 消耗的字节数
 */
int display_utf8_decode(const char *s, uint32_t *cp);

/**
 * @brief 查找字形
 *
 * ASCII 字符返回 8x16 点阵（每行 1 字节），其他字符返回 16x16 点阵
 * （每行 2 字节）。返回 NULL 表示字库中缺少该字形。
 */
const uint8_t *display_font_lookup(uint32_t cp, int *width, int *height);

#ifdef __cplusplus
}
#endif
