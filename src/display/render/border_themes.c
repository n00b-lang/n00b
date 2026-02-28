/*
 * Predefined border theme constants.
 *
 * Migrated from ~/n00b-old/src/text/theme.nc.
 * Field names updated from the old struct layout (horizontal_rule -> horizontal, etc.).
 */

#include "n00b.h"
#include "display/render/types.h"

const n00b_border_theme_t n00b_border_plain = {
    .horizontal  = 0x2500, // ─
    .vertical    = 0x2502, // │
    .upper_left  = 0x250C, // ┌
    .upper_right = 0x2510, // ┐
    .lower_left  = 0x2514, // └
    .lower_right = 0x2518, // ┘
    .cross       = 0x253C, // ┼
    .top_t       = 0x252C, // ┬
    .bottom_t    = 0x2534, // ┴
    .left_t      = 0x251C, // ├
    .right_t     = 0x2524, // ┤
};

const n00b_border_theme_t n00b_border_bold = {
    .horizontal  = 0x2501, // ━
    .vertical    = 0x2503, // ┃
    .upper_left  = 0x250F, // ┏
    .upper_right = 0x2513, // ┓
    .lower_left  = 0x2517, // ┗
    .lower_right = 0x251B, // ┛
    .cross       = 0x254B, // ╋
    .top_t       = 0x2533, // ┳
    .bottom_t    = 0x253B, // ┻
    .left_t      = 0x2523, // ┣
    .right_t     = 0x252B, // ┫
};

const n00b_border_theme_t n00b_border_double = {
    .horizontal  = 0x2550, // ═
    .vertical    = 0x2551, // ║
    .upper_left  = 0x2554, // ╔
    .upper_right = 0x2557, // ╗
    .lower_left  = 0x255A, // ╚
    .lower_right = 0x255D, // ╝
    .cross       = 0x256C, // ╬
    .top_t       = 0x2566, // ╦
    .bottom_t    = 0x2569, // ╩
    .left_t      = 0x2560, // ╠
    .right_t     = 0x2563, // ╣
};

const n00b_border_theme_t n00b_border_dash = {
    .horizontal  = 0x2504, // ┄
    .vertical    = 0x2506, // ┆
    .upper_left  = 0x250C,
    .upper_right = 0x2510,
    .lower_left  = 0x2514,
    .lower_right = 0x2518,
    .cross       = 0x253C,
    .top_t       = 0x252C,
    .bottom_t    = 0x2534,
    .left_t      = 0x251C,
    .right_t     = 0x2524,
};

const n00b_border_theme_t n00b_border_bold_dash = {
    .horizontal  = 0x2505, // ┅
    .vertical    = 0x2507, // ┇
    .upper_left  = 0x250F,
    .upper_right = 0x2513,
    .lower_left  = 0x2517,
    .lower_right = 0x251B,
    .cross       = 0x254B,
    .top_t       = 0x2533,
    .bottom_t    = 0x253B,
    .left_t      = 0x2523,
    .right_t     = 0x252B,
};

const n00b_border_theme_t n00b_border_dash2 = {
    .horizontal  = 0x2508, // ┈
    .vertical    = 0x250A, // ┊
    .upper_left  = 0x250C,
    .upper_right = 0x2510,
    .lower_left  = 0x2514,
    .lower_right = 0x2518,
    .cross       = 0x253C,
    .top_t       = 0x252C,
    .bottom_t    = 0x2534,
    .left_t      = 0x251C,
    .right_t     = 0x2524,
};

const n00b_border_theme_t n00b_border_bold_dash2 = {
    .horizontal  = 0x2509, // ┉
    .vertical    = 0x250B, // ┋
    .upper_left  = 0x250F,
    .upper_right = 0x2513,
    .lower_left  = 0x2517,
    .lower_right = 0x251B,
    .cross       = 0x254B,
    .top_t       = 0x2533,
    .bottom_t    = 0x253B,
    .left_t      = 0x2523,
    .right_t     = 0x252B,
};

const n00b_border_theme_t n00b_border_ascii = {
    .horizontal  = '-',
    .vertical    = '|',
    .upper_left  = '/',
    .upper_right = '\\',
    .lower_left  = '\\',
    .lower_right = '/',
    .cross       = '+',
    .top_t       = '-',
    .bottom_t    = '-',
    .left_t      = '|',
    .right_t     = '|',
};

const n00b_border_theme_t n00b_border_rounded = {
    .horizontal  = 0x2500, // ─
    .vertical    = 0x2502, // │
    .upper_left  = 0x256D, // ╭
    .upper_right = 0x256E, // ╮
    .lower_left  = 0x2570, // ╰
    .lower_right = 0x256F, // ╯
    .cross       = 0x253C, // ┼
    .top_t       = 0x252C, // ┬
    .bottom_t    = 0x2534, // ┴
    .left_t      = 0x251C, // ├
    .right_t     = 0x2524, // ┤
};
