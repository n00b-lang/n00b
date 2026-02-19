/**
 * CLI tool that reads delimited text (stdin or file) and renders it
 * as a styled table using the n00b rendering pipeline.
 *
 * Usage:
 *   n00b_table [-r SEP] [-c SEP] [-s STYLE] [-w WIDTH] [-t THEME] [-h] [FILE]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "strings/string_ops.h"
#include "strings/text_style.h"
#include "strings/style_ops.h"
#include "strings/theme.h"
#include "render/backend.h"
#include "render/canvas.h"
#include "render/plane.h"
#include "table/table.h"

// ====================================================================
// Escape processing for separator arguments
// ====================================================================

/**
 * Process C-style escape sequences in a separator string.
 * Handles: \t \n \r \\
 * Returns a newly allocated (malloc'd) string; caller must free.
 */
static char *
process_escapes(const char *src)
{
    size_t len = strlen(src);
    char  *dst = malloc(len + 1);

    if (!dst) {
        return nullptr;
    }

    size_t j = 0;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len) {
            switch (src[i + 1]) {
            case 't':
                dst[j++] = '\t';
                i++;
                break;
            case 'n':
                dst[j++] = '\n';
                i++;
                break;
            case 'r':
                dst[j++] = '\r';
                i++;
                break;
            case '\\':
                dst[j++] = '\\';
                i++;
                break;
            default:
                dst[j++] = src[i];
                break;
            }
        }
        else {
            dst[j++] = src[i];
        }
    }

    dst[j] = '\0';
    return dst;
}

// ====================================================================
// Style lookup
// ====================================================================

static n00b_table_style_t
lookup_style(const char *name)
{
    if (strcmp(name, "simple") == 0) {
        return n00b_table_style_simple();
    }
    if (strcmp(name, "ornate") == 0) {
        return n00b_table_style_ornate();
    }
    if (strcmp(name, "minimal") == 0) {
        return n00b_table_style_minimal();
    }
    if (strcmp(name, "ascii") == 0) {
        return n00b_table_style_ascii();
    }

    return n00b_table_style_default();
}

// ====================================================================
// Hex color parsing
// ====================================================================

static bool
parse_hex_color(const char *str, n00b_color_t *out)
{
    if (!str) {
        return false;
    }

    // Skip optional '#' prefix.
    if (str[0] == '#') {
        str++;
    }

    char *end  = nullptr;
    long  val  = strtol(str, &end, 16);

    if (end == str || *end != '\0' || val < 0 || val > 0xffffff) {
        return false;
    }

    *out = n00b_color_make((int)val);
    return true;
}

// ====================================================================
// Input reading
// ====================================================================

static char *
read_all(FILE *fp, size_t *out_len)
{
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = malloc(cap);

    if (!buf) {
        return nullptr;
    }

    for (;;) {
        size_t n = fread(buf + len, 1, cap - len, fp);
        len += n;

        if (n == 0) {
            break;
        }

        if (len == cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);

            if (!tmp) {
                free(buf);
                return nullptr;
            }

            buf = tmp;
        }
    }

    *out_len = len;
    return buf;
}

// ====================================================================
// Terminal width detection
// ====================================================================

static int
detect_terminal_width(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }

    return 80;
}

// ====================================================================
// Usage
// ====================================================================

static void
print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS] [FILE]\n"
            "\n"
            "Render delimited text as a styled table.\n"
            "\n"
            "Options:\n"
            "  -r SEP          Row separator (default: \\n)\n"
            "  -c SEP          Column separator (default: ,)\n"
            "  -s STYLE        Style preset: default, simple, ornate, minimal, ascii\n"
            "  -w WIDTH        Override terminal width\n"
            "  -t, --theme NAME  Color theme (default: n00b-dark)\n"
            "  --list-themes   Print available theme names and exit\n"
            "  --no-color      Disable all colors\n"
            "  --no-stripe     Disable alternating row colors\n"
            "  --header-fg HEX Override header foreground (hex RGB, e.g. ff0000)\n"
            "  --header-bg HEX Override header background\n"
            "  --cell-fg HEX   Override cell foreground\n"
            "  --cell-bg HEX   Override cell background\n"
            "  -h              Show this help message\n"
            "\n"
            "Escape sequences (\\t \\n \\r \\\\) are recognized in separator arguments.\n"
            "If FILE is omitted, reads from stdin.\n",
            prog);
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    const char *row_sep_raw  = nullptr;
    const char *col_sep_raw  = nullptr;
    const char *style_name   = "default";
    const char *theme_name   = nullptr;
    int         width        = 0;
    const char *filename     = nullptr;
    bool        no_color     = false;
    bool        no_stripe    = false;
    bool        list_themes  = false;

    // Color override storage.
    n00b_color_t header_fg = 0;
    n00b_color_t header_bg = 0;
    n00b_color_t cell_fg   = 0;
    n00b_color_t cell_bg   = 0;

    // Manual argument parsing.
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-r") == 0
                 || strcmp(argv[i], "--row-sep") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: -r requires an argument\n");
                return 1;
            }
            row_sep_raw = argv[i];
        }
        else if (strcmp(argv[i], "-c") == 0
                 || strcmp(argv[i], "--col-sep") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: -c requires an argument\n");
                return 1;
            }
            col_sep_raw = argv[i];
        }
        else if (strcmp(argv[i], "-s") == 0
                 || strcmp(argv[i], "--style") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: -s requires an argument\n");
                return 1;
            }
            style_name = argv[i];
        }
        else if (strcmp(argv[i], "-w") == 0
                 || strcmp(argv[i], "--width") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: -w requires an argument\n");
                return 1;
            }
            width = atoi(argv[i]);

            if (width <= 0) {
                fprintf(stderr, "Error: invalid width '%s'\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-t") == 0
                 || strcmp(argv[i], "--theme") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: -t/--theme requires an argument\n");
                return 1;
            }
            theme_name = argv[i];
        }
        else if (strcmp(argv[i], "--list-themes") == 0) {
            list_themes = true;
        }
        else if (strcmp(argv[i], "--no-color") == 0) {
            no_color = true;
        }
        else if (strcmp(argv[i], "--no-stripe") == 0) {
            no_stripe = true;
        }
        else if (strcmp(argv[i], "--header-fg") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --header-fg requires an argument\n");
                return 1;
            }
            if (!parse_hex_color(argv[i], &header_fg)) {
                fprintf(stderr, "Error: invalid color '%s'\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--header-bg") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --header-bg requires an argument\n");
                return 1;
            }
            if (!parse_hex_color(argv[i], &header_bg)) {
                fprintf(stderr, "Error: invalid color '%s'\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--cell-fg") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --cell-fg requires an argument\n");
                return 1;
            }
            if (!parse_hex_color(argv[i], &cell_fg)) {
                fprintf(stderr, "Error: invalid color '%s'\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--cell-bg") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --cell-bg requires an argument\n");
                return 1;
            }
            if (!parse_hex_color(argv[i], &cell_bg)) {
                fprintf(stderr, "Error: invalid color '%s'\n", argv[i]);
                return 1;
            }
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
        else {
            filename = argv[i];
        }
    }

    // Handle --list-themes.
    if (list_themes) {
        int          count = 0;
        const char **names = n00b_theme_list(&count);

        for (int i = 0; i < count; i++) {
            fprintf(stdout, "%s\n", names[i]);
        }

        return 0;
    }

    // Set theme before building the table (theme affects style presets).
    if (theme_name) {
        if (!n00b_theme_set_current(theme_name)) {
            fprintf(stderr, "Error: unknown theme '%s'\n", theme_name);
            fprintf(stderr, "Use --list-themes to see available themes.\n");
            return 1;
        }
    }

    // Read input.
    FILE *fp = stdin;

    if (filename) {
        fp = fopen(filename, "r");

        if (!fp) {
            perror(filename);
            return 1;
        }
    }

    size_t input_len = 0;
    char  *input_raw = read_all(fp, &input_len);

    if (filename) {
        fclose(fp);
    }

    if (!input_raw || input_len == 0) {
        free(input_raw);
        fprintf(stderr, "Error: no input\n");
        return 1;
    }

    // Build n00b_string_t from raw input.
    n00b_string_t input = n00b_string_from_raw(nullptr, input_raw,
                                                (int64_t)input_len,
                                                (int64_t)input_len);
    free(input_raw);

    // Process separator arguments.
    n00b_string_t  rsep_val;
    n00b_string_t *rsep_ptr = nullptr;

    if (row_sep_raw) {
        char *processed = process_escapes(row_sep_raw);
        rsep_val = n00b_string_from_raw(nullptr, processed,
                                         (int64_t)strlen(processed),
                                         (int64_t)strlen(processed));
        free(processed);
        rsep_ptr = &rsep_val;
    }

    n00b_string_t  csep_val;
    n00b_string_t *csep_ptr = nullptr;

    if (col_sep_raw) {
        char *processed = process_escapes(col_sep_raw);
        csep_val = n00b_string_from_raw(nullptr, processed,
                                         (int64_t)strlen(processed),
                                         (int64_t)strlen(processed));
        free(processed);
        csep_ptr = &csep_val;
    }

    // Look up style preset.
    n00b_table_style_t style = lookup_style(style_name);

    // Handle --no-color: strip all styles from presets.
    if (no_color) {
        if (style.table_props) {
            style.table_props->fill_style   = nullptr;
            style.table_props->text_style   = nullptr;
            style.table_props->border_style = nullptr;
        }
        if (style.cell_props) {
            style.cell_props->fill_style = nullptr;
            style.cell_props->text_style = nullptr;
        }
        if (style.header_props) {
            style.header_props->fill_style = nullptr;
            style.header_props->text_style = nullptr;
        }
        if (style.alt_cell_props) {
            style.alt_cell_props->fill_style = nullptr;
            style.alt_cell_props->text_style = nullptr;
        }
    }

    // Apply color overrides.
    if (n00b_color_is_set(header_fg) && style.header_props) {
        if (!style.header_props->text_style) {
            style.header_props->text_style = n00b_str_style_new();
        }
        style.header_props->text_style->fg_rgb        = header_fg;
        style.header_props->text_style->fg_palette_ix  = N00B_PAL_UNSET;
    }
    if (n00b_color_is_set(header_bg) && style.header_props) {
        if (!style.header_props->fill_style) {
            style.header_props->fill_style = n00b_str_style_new();
        }
        style.header_props->fill_style->bg_rgb        = header_bg;
        style.header_props->fill_style->bg_palette_ix  = N00B_PAL_UNSET;
    }
    if (n00b_color_is_set(cell_fg) && style.cell_props) {
        if (!style.cell_props->text_style) {
            style.cell_props->text_style = n00b_str_style_new();
        }
        style.cell_props->text_style->fg_rgb        = cell_fg;
        style.cell_props->text_style->fg_palette_ix  = N00B_PAL_UNSET;
    }
    if (n00b_color_is_set(cell_bg) && style.cell_props) {
        if (!style.cell_props->fill_style) {
            style.cell_props->fill_style = n00b_str_style_new();
        }
        style.cell_props->fill_style->bg_rgb        = cell_bg;
        style.cell_props->fill_style->bg_palette_ix  = N00B_PAL_UNSET;
    }

    // Build the table from the input string.
    n00b_table_t *table = n00b_table_from_string(
        input,
        .row_sep      = rsep_ptr,
        .col_sep      = csep_ptr,
        .table_props  = style.table_props,
        .cell_props   = style.cell_props,
        .header_props = style.header_props,
        .alt_props    = style.alt_cell_props,
        .no_stripe    = no_stripe ? 1 : 0);

    // Detect output width.
    if (width == 0) {
        width = detect_terminal_width();
    }

    // Render the table into a plane.
    n00b_plane_t *plane = n00b_table_render(table, (int64_t)width);

    if (!plane) {
        fprintf(stderr, "Error: table produced no output\n");
        n00b_table_destroy(table);
        return 1;
    }

    // Output via canvas + inline ANSI backend (no cursor positioning).
    n00b_canvas_t *canvas = n00b_canvas_new(&n00b_renderer_ansi_inline);
    n00b_canvas_resize(canvas, plane->total_rows, plane->total_cols);
    n00b_canvas_add_plane(canvas, plane);
    n00b_canvas_render(canvas);
    n00b_canvas_flush(canvas);

    n00b_canvas_destroy(canvas);
    n00b_table_destroy(table);

    return 0;
}
