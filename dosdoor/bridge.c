/* bridge.c - file-based IPC between the DOS door and the Win32 helper. */
#include "common.h"
#include "bridge.h"

static long g_in_pos = 0;
static int  g_node   = 0;   /* 0 = no suffix (ANETIRC.OUT), 1-8 = suffix */

static void bridge_out_name(char *buf, int bufsz) {
    if (g_node > 0)
        snprintf(buf, (size_t)bufsz, "ANETIRC%d.OUT", g_node);
    else
        safe_copy(buf, (size_t)bufsz, "ANETIRC.OUT");
}

static void bridge_in_name(char *buf, int bufsz) {
    if (g_node > 0)
        snprintf(buf, (size_t)bufsz, "ANETIRC%d.IN", g_node);
    else
        safe_copy(buf, (size_t)bufsz, "ANETIRC.IN");
}

void bridge_set_node(int n) {
    g_node = (n >= 2 && n <= 8) ? n : 0;
    g_in_pos = 0;
}

int bridge_connect_local(void) {
    FILE *fp;
    char fname[32];
    bridge_in_name(fname, sizeof(fname));
    fp = fopen(fname, "r");
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0)
            g_in_pos = ftell(fp);
        if (g_in_pos < 0) g_in_pos = 0;
        fclose(fp);
    } else {
        g_in_pos = 0;
    }
    return 1;
}

void bridge_close(void) {}

int bridge_send_line(const char *line) {
    FILE *fp;
    char fname[32];
    if (!line) return 0;
    bridge_out_name(fname, sizeof(fname));
    fp = fopen(fname, "a");
    if (!fp) return 0;
    fprintf(fp, "%s\n", line);
    fclose(fp);
    return 1;
}

int bridge_poll_line(char *out, int outsz) {
    FILE *fp;
    char buf[512];
    char fname[32];
    long endpos;

    bridge_in_name(fname, sizeof(fname));
    fp = fopen(fname, "r");
    if (!fp) return 0;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    endpos = ftell(fp);
    if (endpos < 0) { fclose(fp); return 0; }
    if (g_in_pos > endpos) g_in_pos = 0;

    if (fseek(fp, g_in_pos, SEEK_SET) != 0) { fclose(fp); return 0; }
    if (!fgets(buf, sizeof(buf), fp))        { fclose(fp); return 0; }

    g_in_pos = ftell(fp);
    fclose(fp);

    trim_crlf(buf);
    safe_copy(out, (size_t)outsz, buf);
    return 1;
}
