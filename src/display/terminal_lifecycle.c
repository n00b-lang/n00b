#include <signal.h>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

#include "n00b.h"
#include "display/render/canvas.h"
#include "internal/display/backend_services.h"
#include "internal/display/terminal_lifecycle.h"

#ifndef _WIN32
static struct termios g_saved_termios;
static bool           g_termios_saved = false;
static bool           g_manages_tty   = false;

static void tty_write_raw(const char *seq);

static void
signal_cleanup_handler(int sig)
{
    if (!g_manages_tty) {
        static const char mouse_off[]   = "\033[?1006l\033[?1002l\033[?1000l";
        static const char show_cursor[] = "\033[?25h";
        static const char sgr_reset[]   = "\033[0m";
        static const char alt_leave[]   = "\033[?1049l";

        write(STDOUT_FILENO, mouse_off, sizeof(mouse_off) - 1);
        write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
        write(STDOUT_FILENO, sgr_reset, sizeof(sgr_reset) - 1);
        write(STDOUT_FILENO, alt_leave, sizeof(alt_leave) - 1);
    }

    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
        g_termios_saved = false;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void
tty_write_raw(const char *seq)
{
    size_t len = strlen(seq);
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, seq, len);
        if (n <= 0) {
            break;
        }
        seq += (size_t)n;
        len -= (size_t)n;
    }
}
#endif

void
n00b_display_terminal_setup(n00b_canvas_t *canvas)
{
#ifdef _WIN32
    (void)canvas;
#else
    n00b_render_cap_t caps = n00b_display_backend_caps(canvas);
    g_manages_tty = (caps & N00B_RCAP_MANAGES_TTY) != 0;

    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
        g_termios_saved = true;
    }

    struct sigaction sa = {
        .sa_handler = signal_cleanup_handler,
        .sa_flags   = 0,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    if (g_manages_tty || !isatty(STDIN_FILENO)) {
        return;
    }

    if (g_termios_saved) {
        struct termios raw = g_saved_termios;
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }

    n00b_canvas_alt_screen_enter(canvas);

    if (caps & N00B_RCAP_MOUSE) {
        tty_write_raw("\033[?1000h");
        tty_write_raw("\033[?1002h");
        tty_write_raw("\033[?1006h");
    }
#endif
}

void
n00b_display_terminal_teardown(n00b_canvas_t *canvas)
{
#ifdef _WIN32
    (void)canvas;
#else
    (void)canvas;

    if (!g_manages_tty) {
        tty_write_raw("\033[?1006l");
        tty_write_raw("\033[?1002l");
        tty_write_raw("\033[?1000l");
        tty_write_raw("\033[?25h");
        tty_write_raw("\033[0m");
        tty_write_raw("\033[?1049l");
    }

    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_termios_saved = false;
    }

    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    g_manages_tty = false;
#endif
}
