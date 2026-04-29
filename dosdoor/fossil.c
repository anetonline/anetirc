/* fossil.c - INT 14h FOSSIL serial I/O plus a local-keyboard fallback. */
#include "common.h"
#include "fossil.h"
#include <dos.h>
#include <conio.h>

static int g_fossil_ok = 0;
static unsigned short g_fossil_port = 0;
static int g_local_mode = 0;
static int g_local_pending = -1;

static unsigned short fossil_int14(unsigned char ah, unsigned char al,
                                    unsigned short dx,
                                    unsigned short *out_ax,
                                    unsigned short *out_bx) {
    union REGS inregs, outregs;
    inregs.h.ah = ah; inregs.h.al = al; inregs.x.dx = dx;
    int86(0x14, &inregs, &outregs);
    if (out_ax) *out_ax = outregs.x.ax;
    if (out_bx) *out_bx = outregs.x.bx;
    return outregs.x.ax;
}

void fossil_set_port(unsigned short port) { g_fossil_port = port; }
void fossil_set_local_mode(void) { g_local_mode = 1; }

int fossil_init(void) {
    unsigned short ax = 0, bx = 0;
    if (g_local_mode) { g_fossil_ok = 1; return 1; }
    fossil_int14(0x04, 0, g_fossil_port, &ax, &bx);
    g_fossil_ok = 1;
    return g_fossil_ok;
}

void fossil_deinit(void) {
    unsigned short ax = 0, bx = 0;
    if (!g_fossil_ok) return;
    if (!g_local_mode)
        fossil_int14(0x05, 0, g_fossil_port, &ax, &bx);
    g_fossil_ok = 0;
}

int fossil_getch_nonblock(void) {
    unsigned short ax = 0, bx = 0;
    int c;
    if (!g_fossil_ok) return -1;
    if (g_local_mode) {
        if (g_local_pending >= 0) {
            c = g_local_pending;
            g_local_pending = -1;
            return c;
        }
        if (!kbhit()) return -1;
        c = getch();
        if (c == 0 || c == 0xE0) {
            if (kbhit()) g_local_pending = getch();
            return 0;
        }
        return c;
    }
    fossil_int14(0x03, 0, g_fossil_port, &ax, &bx);
    if ((ax & 0x0100u) == 0) return -1;
    fossil_int14(0x02, 0, g_fossil_port, &ax, &bx);
    return (int)(ax & 0x00FFu);
}

int fossil_getch_block(void) {
    unsigned short ax = 0, bx = 0;
    if (!g_fossil_ok) return -1;
    if (g_local_mode) return getch();
    fossil_int14(0x02, 0, g_fossil_port, &ax, &bx);
    return (int)(ax & 0x00FFu);
}

void fossil_putch(int ch) {
    unsigned short ax = 0, bx = 0;
    if (!g_fossil_ok) return;
    if (g_local_mode) { putchar(ch & 0xFF); return; }
    fossil_int14(0x01, (unsigned char)(ch & 0xFF), g_fossil_port, &ax, &bx);
}

void fossil_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') fossil_putch('\r');
        fossil_putch((unsigned char)*s++);
    }
}

void fossil_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    fossil_puts(buf);
}

void fossil_cls(void) { fossil_puts("\033[2J\033[H"); }
