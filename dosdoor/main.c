/* main.c - ANETIRC DOS door: handles handle/nick entry, chat UI, slash commands,
 * FOSSIL serial I/O, and the bridge-file protocol. */

#include "common.h"
#include "fossil.h"
#include "dropfile.h"
#include "bridge.h"
#include <time.h>
#include <dos.h>

/* Forward declarations */
static void fossil_put_pipe(const char *s);
static void draw_color_bar(void);
static void put_input_colored(void);
static void redraw_chat_input(void);

/* ============================================================================
   CONSTANTS
   ============================================================================ */

#define MSG_MAX         80    /* ring buffer for pre-wrapped lines (DGROUP budget) */
#define MSG_LEN        200
#define INPUT_RULE_ROW  23
#define INPUT_PROMPT_ROW 24
#define SCREEN_W        80
#define CHAT_W          78
#define CHAT_MSG_TOP     4
#define CHAT_MSG_BOT    22
#define CHAT_MSG_ROWS   19

/* Fallback IRC server if IRCBBS.DAT does not provide one. */
#define IRC_SERVER "irc.libera.chat"
#define IRC_PORT    6667

#define TWIT_MAX 10
typedef struct {
    char nick[24];             /* IRC nick (may differ from BBS login) */
    int  handle_color;
    char handle_prefix[32];
    char handle_suffix[32];
    int  text_color;
    char quitmsg[64];          /* QUIT reason */
    char partmsg[64];          /* PART reason */
    int  theme;
    char twit_list[TWIT_MAX][24];
    int  twit_count;
} user_settings_t;

typedef enum {
    MODE_HANDLE   = 0,
    MODE_MENU     = 1,
    MODE_SETTINGS = 2,
    MODE_STATS    = 3,
    MODE_CHAT     = 4,
    MODE_HELP     = 5,
    MODE_NETWORKS = 6    /* Network picker */
} app_mode_t;

/* Well-known IRC networks shown in the "Choose Network" menu. If both
 * port_plain and port_tls are nonzero, the user is asked Y/N for TLS
 * after picking the letter. */
typedef struct {
    const char *label;
    const char *host;
    int port_plain;     /* 0 = not available */
    int port_tls;       /* 0 = not available */
} irc_network_t;

static const irc_network_t g_networks[] = {
    { "Synchronet",        "irc.synchro.net",         6667, 6697 },
    { "Slackers",          "slackers.ddns.net",       6667, 0    },
    { "Bottomless Abyss",  "mrc.bottomlessabyss.net", 0   , 6697 },
    { "Libera.Chat",       "irc.libera.chat",         6667, 6697 },
    { "OFTC",              "irc.oftc.net",            6667, 6697 },
    { "EFnet",             "irc.efnet.org",           6667, 0    },
    { "DALnet",            "irc.dal.net",             6667, 6697 },
    { "Undernet",          "irc.undernet.org",        6667, 0    },
    { "Rizon",             "irc.rizon.net",           6667, 6697 },
    { "IRCnet",            "open.ircnet.net",         6667, 6697 },
    { "QuakeNet",          "irc.quakenet.org",        6667, 0    }
};
#define NUM_NETWORKS ((int)(sizeof(g_networks) / sizeof(g_networks[0])))

/* Sub-state inside MODE_NETWORKS. */
typedef enum {
    NS_PICK        = 0,   /* list shown, waiting for letter */
    NS_TLS_PROMPT  = 1,   /* ask Y/N for TLS on picked network */
    NS_CUSTOM_HOST = 2,   /* manual host entry */
    NS_CUSTOM_PORT = 3,   /* manual port entry */
    NS_CUSTOM_TLS  = 4    /* manual TLS Y/N */
} net_substate_t;

typedef enum {
    EDIT_NONE       = 0,
    EDIT_HANDLE     = 1,
    EDIT_HCOLOR     = 2,
    EDIT_PREFIX     = 3,
    EDIT_SUFFIX     = 4,
    EDIT_QUITMSG    = 5,
    EDIT_PARTMSG    = 6,
    EDIT_TEXTCOLOR  = 7
} edit_field_t;

/* ============================================================================
   GLOBALS
   ============================================================================ */

static char g_msgs[MSG_MAX][MSG_LEN];
static int  g_msg_count = 0;

#define SENT_HIST_MAX 10
static char g_sent_hist[SENT_HIST_MAX][128];
static int  g_sent_hist_count = 0;
static int  g_hist_browse = -1;
static char g_hist_saved[128];

static app_mode_t  g_mode     = MODE_HANDLE;
static edit_field_t g_edit    = EDIT_NONE;
static int         g_sent_quit = 0;

static char g_input[512];
static int  g_input_len = 0;
static int  g_input_max = 400;

static user_settings_t g_settings;
static char g_bbs_user[64];

/* Chat header state */
static char g_chat_chan[32];
static char g_chat_topic[80];
static char g_chat_users[16];
static char g_chat_status[32];
static char g_chat_latency[16];

static int g_scroll_off = 0;
static int g_new_while_scrolled = 0;
static int g_chat_view_w = 60;

/* Twit filter uses extract_sender; no mention counter in IRC build. */

static char g_last_dm_target[32];
static int  g_return_to_menu = 0;

static char g_tab_users[512];
static int g_tab_hl_start = -1;
static int g_tab_hl_end   = -1;

static int g_local_mode = 0;
static int g_help_page = 0;

/* Network picker state */
static int            g_net_idx = -1;       /* selected network, -1 = none */
static net_substate_t g_net_sub = NS_PICK;
static char           g_net_custom_host[128];
static int            g_net_custom_port = 6667;

/* ============================================================================
   SETTINGS DATABASE  (IRCUSER.DAT)
   ============================================================================ */

static void make_section_key(char *out, size_t outsz, const char *name) {
    size_t i = 0;
    while (*name && i + 1 < outsz) {
        unsigned char c = (unsigned char)*name++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) {
            out[i++] = (char)c;
        } else if (c == ' ' || c == '_') {
            out[i++] = '_';
        }
    }
    out[i] = 0;
    if (!out[0]) { out[0] = 'U'; out[1] = 0; }
}

static void settings_defaults(user_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->nick[0]   = 0;
    s->handle_color = 11;
    s->text_color   = 7;
    safe_copy(s->quitmsg, sizeof(s->quitmsg), "Leaving");
    safe_copy(s->partmsg, sizeof(s->partmsg), "bye");
    s->theme = 1;
    s->twit_count = 0;
}

static int load_settings(const char *bbs_username, user_settings_t *out) {
    FILE *fp;
    char line[128];
    char section[64];
    char key_section[64];
    int in_section = 0;

    settings_defaults(out);
    make_section_key(key_section, sizeof(key_section), bbs_username);

    fp = fopen("IRCUSER.DAT", "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        trim_crlf(line);
        if (!line[0] || line[0] == '#') continue;

        if (line[0] == '[') {
            size_t i;
            for (i = 0; i + 1 < sizeof(section) && line[i + 1] && line[i + 1] != ']'; ++i)
                section[i] = line[i + 1];
            section[i] = 0;
            in_section = (stricmp(section, key_section) == 0);
            continue;
        }

        if (!in_section) continue;

        {
            char *eq = strchr(line, '=');
            const char *k, *v;
            if (!eq) continue;
            *eq = 0;
            k = line;
            v = eq + 1;

            if (stricmp(k, "nick") == 0)
                safe_copy(out->nick, sizeof(out->nick), v);
            else if (stricmp(k, "handle_color") == 0)
                out->handle_color = atoi(v);
            else if (stricmp(k, "handle_prefix") == 0)
                safe_copy(out->handle_prefix, sizeof(out->handle_prefix), v);
            else if (stricmp(k, "handle_suffix") == 0)
                safe_copy(out->handle_suffix, sizeof(out->handle_suffix), v);
            else if (stricmp(k, "text_color") == 0)
                out->text_color = atoi(v);
            else if (stricmp(k, "quitmsg") == 0)
                safe_copy(out->quitmsg, sizeof(out->quitmsg), v);
            else if (stricmp(k, "partmsg") == 0)
                safe_copy(out->partmsg, sizeof(out->partmsg), v);
            else if (stricmp(k, "theme") == 0)
                out->theme = atoi(v);
            else if (stricmp(k, "twit") == 0) {
                char tmp[256], *tok;
                safe_copy(tmp, sizeof(tmp), v);
                tok = strtok(tmp, ",");
                while (tok && out->twit_count < TWIT_MAX) {
                    while (*tok == ' ') tok++;
                    if (*tok) {
                        safe_copy(out->twit_list[out->twit_count],
                                  sizeof(out->twit_list[0]), tok);
                        out->twit_count++;
                    }
                    tok = strtok(NULL, ",");
                }
            }
        }
    }

    fclose(fp);
    return out->nick[0] ? 1 : 0;
}

static void write_settings_section(FILE *fp, const char *key_section,
                                   const user_settings_t *s) {
    fprintf(fp, "[%s]\n", key_section);
    fprintf(fp, "nick=%s\n",           s->nick);
    fprintf(fp, "handle_color=%d\n",   s->handle_color);
    fprintf(fp, "handle_prefix=%s\n",  s->handle_prefix);
    fprintf(fp, "handle_suffix=%s\n",  s->handle_suffix);
    fprintf(fp, "text_color=%d\n",     s->text_color);
    fprintf(fp, "quitmsg=%s\n",        s->quitmsg);
    fprintf(fp, "partmsg=%s\n",        s->partmsg);
    fprintf(fp, "theme=%d\n",          s->theme);
    if (s->twit_count > 0) {
        int ti;
        fprintf(fp, "twit=");
        for (ti = 0; ti < s->twit_count; ti++) {
            if (ti > 0) fprintf(fp, ",");
            fprintf(fp, "%s", s->twit_list[ti]);
        }
        fprintf(fp, "\n");
    }
    fprintf(fp, "\n");
}

static void save_settings(const char *bbs_username, const user_settings_t *s) {
    char key_section[64];
    char line[128];
    FILE *fin, *fout;
    int in_target = 0, wrote = 0;

    make_section_key(key_section, sizeof(key_section), bbs_username);

    fin  = fopen("IRCUSER.DAT", "r");
    fout = fopen("IRCUSER.TMP", "w");
    if (!fout) { if (fin) fclose(fin); return; }

    if (fin) {
        while (fgets(line, sizeof(line), fin)) {
            trim_crlf(line);
            if (line[0] == '[') {
                char sec[64];
                size_t j = 0;
                while (j + 1 < sizeof(sec) && line[j+1] && line[j+1] != ']')
                    { sec[j] = line[j+1]; j++; }
                sec[j] = 0;
                if (stricmp(sec, key_section) == 0) {
                    write_settings_section(fout, key_section, s);
                    wrote = 1;
                    in_target = 1;
                    continue;
                } else {
                    in_target = 0;
                }
            }
            if (!in_target)
                fprintf(fout, "%s\n", line);
        }
        fclose(fin);
    }

    if (!wrote)
        write_settings_section(fout, key_section, s);

    fclose(fout);
    remove("IRCUSER.DAT");
    rename("IRCUSER.TMP", "IRCUSER.DAT");
}

/* ============================================================================
   DISPLAY PRIMITIVES
   ============================================================================ */

static void ansi_goto(int row, int col) {
    fossil_printf("\033[%d;%dH", row, col);
}

static void ansi_color_code(int n) {
    static const int fgbase[8] = { 30, 34, 32, 36, 31, 35, 33, 37 };
    static const int bgmap[3]  = { 40, 44, 42 };
    if (n < 0) n = 0;
    if (n <= 7) {
        fossil_printf("\033[22;%dm", fgbase[n]);
    } else if (n <= 15) {
        fossil_printf("\033[1;%dm", fgbase[n - 8]);
    } else if (n <= 18) {
        fossil_printf("\033[%dm", bgmap[n - 16]);
    } else {
        fossil_printf("\033[1;%dm", fgbase[7]);
    }
}

static void fossil_put_pipe(const char *s) {
    while (s && *s) {
        if (*s == '|' &&
            isdigit((unsigned char)s[1]) &&
            isdigit((unsigned char)s[2])) {
            ansi_color_code((s[1] - '0') * 10 + (s[2] - '0'));
            s += 3;
            continue;
        }
        fossil_putch((unsigned char)*s++);
    }
    fossil_puts("\033[0m");
}

static void draw_hr(void) {
    int i, t;
    static const char *theme_ansi[7] = {
        "\033[90m","\033[90m","\033[34m","\033[32m",
        "\033[36m","\033[31m","\033[35m"
    };
    t = g_settings.theme;
    if (t < 1 || t > 6) t = 1;
    fossil_puts(theme_ansi[t]);
    for (i = 0; i < SCREEN_W; ++i)
        fossil_putch(0xDC);
    fossil_puts("\033[0m\r\n");
}

static void draw_info_line(const char *label, const char *value) {
    char buf[256];
    snprintf(buf, sizeof(buf), "|15%-14s|08: |07%s", label ? label : "", value ? value : "");
    fossil_put_pipe(buf);
    fossil_puts("\r\n");
}

static void draw_color_bar(void) {
    char bar[300];
    int i, pos = 0;
    const char *header = "|08Colors: ";
    size_t hlen = strlen(header);
    memcpy(bar, header, hlen);
    pos = (int)hlen;
    for (i = 1; i <= 15 && pos + 10 < (int)sizeof(bar); ++i) {
        int n = snprintf(bar + pos, sizeof(bar) - pos, "|%02d%-3d", i, i);
        if (n > 0) pos += n;
    }
    bar[pos] = 0;
    fossil_put_pipe(bar);
    fossil_puts("\r\n");
}

static void draw_handle_preview(void) {
    char preview[128];
    snprintf(preview, sizeof(preview), "|07Preview: %s|%02d%s%s|07",
             g_settings.handle_prefix,
             g_settings.handle_color,
             g_settings.nick[0] ? g_settings.nick : "Nick",
             g_settings.handle_suffix);
    fossil_put_pipe(preview);
    fossil_puts("\r\n");
}

/* ============================================================================
   TWIT (IGNORE) LIST HELPERS
   ============================================================================ */

static int twit_has(const char *handle) {
    int i;
    for (i = 0; i < g_settings.twit_count; i++)
        if (stricmp(g_settings.twit_list[i], handle) == 0) return 1;
    return 0;
}

static int twit_add(const char *handle) {
    if (twit_has(handle)) return 0;
    if (g_settings.twit_count >= TWIT_MAX) return -1;
    safe_copy(g_settings.twit_list[g_settings.twit_count],
              sizeof(g_settings.twit_list[0]), handle);
    g_settings.twit_count++;
    save_settings(g_bbs_user, &g_settings);
    return 1;
}

static int twit_remove(const char *handle) {
    int i;
    for (i = 0; i < g_settings.twit_count; i++) {
        if (stricmp(g_settings.twit_list[i], handle) == 0) {
            int j;
            for (j = i; j < g_settings.twit_count - 1; j++)
                memcpy(g_settings.twit_list[j], g_settings.twit_list[j+1],
                       sizeof(g_settings.twit_list[0]));
            g_settings.twit_count--;
            save_settings(g_bbs_user, &g_settings);
            return 1;
        }
    }
    return 0;
}

/* ============================================================================
   MESSAGE BUFFER
   ============================================================================ */

static int copy_vis_pipe(char *dst, int dstsz, const char *src, int max_vis,
                         int *active_color_io) {
    int di = 0, vis = 0, si = 0;
    int last_space_dst = -1, last_space_src = -1;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = 0;

    while (src[si] && di + 1 < dstsz) {
        if (src[si] == '|' &&
            isdigit((unsigned char)src[si + 1]) &&
            isdigit((unsigned char)src[si + 2])) {
            int c = (src[si + 1] - '0') * 10 + (src[si + 2] - '0');
            if (di + 3 >= dstsz) break;
            dst[di++] = src[si++];
            dst[di++] = src[si++];
            dst[di++] = src[si++];
            if (active_color_io) *active_color_io = c;
            continue;
        }
        if (vis >= max_vis) break;
        if (src[si] == ' ') { last_space_dst = di; last_space_src = si; }
        dst[di++] = src[si++];
        ++vis;
    }
    dst[di] = 0;

    if (src[si] && last_space_dst > 0) {
        dst[last_space_dst] = 0;
        return last_space_src + 1;
    }
    return si;
}

static void add_msg(const char *s) {
    int i;
    if (!s || !*s) return;
    if (g_msg_count >= MSG_MAX) {
        for (i = 1; i < MSG_MAX; ++i)
            safe_copy(g_msgs[i - 1], sizeof(g_msgs[i - 1]), g_msgs[i]);
        g_msg_count = MSG_MAX - 1;
    }
    safe_copy(g_msgs[g_msg_count], sizeof(g_msgs[g_msg_count]), s);
    g_msg_count++;
}

static void add_msg_wrapped(const char *s, int width) {
    char line[MSG_LEN];
    int off = 0, slen;
    int active_color = -1;

    if (!s || !*s) { add_msg(""); return; }
    slen = (int)strlen(s);

    while (off < slen) {
        int consumed, dst_start = 0;

        while (s[off] == ' ') off++;
        if (off >= slen) break;

        if (active_color >= 0 && off > 0) {
            line[0] = '|';
            line[1] = '0' + active_color / 10;
            line[2] = '0' + active_color % 10;
            dst_start = 3;
        }

        consumed = copy_vis_pipe(line + dst_start,
                                 (int)sizeof(line) - dst_start,
                                 s + off, width, &active_color);
        if (consumed <= 0) break;
        add_msg(line);
        off += consumed;
    }
}

/* ============================================================================
   SCREEN LAYOUTS
   ============================================================================ */

static void draw_header(const char *subtitle) {
    fossil_cls();
    fossil_put_pipe("|10A|11NET|15IRC|07  |08Internet Relay Chat for DOS BBSes|07\r\n");
    draw_hr();
    if (subtitle && *subtitle) {
        fossil_put_pipe(subtitle);
        fossil_puts("\r\n");
        draw_hr();
    }
}

static void draw_handle_entry(const dropfile_info_t *drop) {
    draw_header("|15Welcome to ANET IRC|07");
    draw_info_line("BBS Login", drop->alias[0] ? drop->alias : drop->user_name);
    fossil_put_pipe("|07\r\n");
    fossil_put_pipe("|07Your |15IRC nickname|07 is how others see you on IRC.\r\n");
    fossil_put_pipe("|07It may differ from your BBS login name.\r\n");
    fossil_put_pipe("|07  - Max 20 characters\r\n");
    fossil_put_pipe("|07  - First char must be a letter\r\n");
    fossil_put_pipe("|07  - Letters, digits, and - _ \\ [ ] { } ^ ` only\r\n");
    fossil_puts("\r\n");
    draw_hr();
    fossil_put_pipe("|15Enter your IRC nick:|07\r\n");
    fossil_put_pipe("|08> |07");
    fossil_put_pipe(g_input);
    fossil_putch('_');
}

static void draw_help(void) {
    draw_header("|14Help|07");
    draw_hr();

    if (g_help_page == 0) {
        fossil_put_pipe("|14  ANET IRC Help\r\n\r\n|07");
        fossil_put_pipe("|08 |151|07) About IRC & ANETIRC\r\n");
        fossil_put_pipe("|08 |152|07) Channels, Chat & Navigation\r\n");
        fossil_put_pipe("|08 |153|07) Server, Users & Modes\r\n");
        fossil_put_pipe("|08 |154|07) Services, Sessions & CTCP\r\n");
        fossil_put_pipe("\r\n|08Press |151-4|07 or |15B|07 to return: ");

    } else if (g_help_page == 1) {
        fossil_put_pipe("|14  About IRC & ANETIRC\r\n\r\n|07");
        fossil_put_pipe("|15IRC|07 (Internet Relay Chat) is the classic realtime chat\r\n");
        fossil_put_pipe("|07protocol. Servers host channels (#foo) where users gather.\r\n\r\n");
        fossil_put_pipe("|11Protocol:|07 RFC 1459/2812  |11Client:|07 ANETIRC 1.0\r\n\r\n");
        fossil_put_pipe("|15Quick start:\r\n|07");
        fossil_put_pipe(" /join #channel       Join a channel\r\n");
        fossil_put_pipe(" /msg nick text       Private message\r\n");
        fossil_put_pipe(" /nick newname        Change nick\r\n");
        fossil_put_pipe(" /list                Channel directory\r\n");
        fossil_put_pipe(" /whois nick          Look up a user\r\n");
        fossil_put_pipe(" /users               Popup: users in current channel\r\n");
        fossil_put_pipe(" /help                Full command list\r\n");
        draw_hr();
        fossil_put_pipe("|08Press any key... ");

    } else if (g_help_page == 2) {
        fossil_put_pipe("|14  Channels & Chat\r\n\r\n|07");
        fossil_put_pipe("|15/join #chan [key] |08or |15/j      /part [#chan] |08or |15/leave\r\n|07");
        fossil_put_pipe("|15/hop |07(part+rejoin)   |15/cycle    /win #chan  |07(switch)\r\n");
        fossil_put_pipe("|15/list    /names [#ch]   /who [tgt]    /whois nick\r\n|07");
        fossil_put_pipe("|15/msg nick text |08or|15 /t nick text   /query nick\r\n|07");
        fossil_put_pipe("|15/notice nick text   /r text |07(reply last PM)\r\n");
        fossil_put_pipe("|15/me <text>           /topic [text]\r\n|07");
        fossil_put_pipe("|15/away [msg]          /back\r\n|07");
        fossil_put_pipe("|15/users |08or |15/u  |07(popup user list)   |15/clear |07(scrollback)\r\n");
        fossil_put_pipe("|15/quit [reason]\r\n\r\n|07");
        fossil_put_pipe("|11Navigation:\r\n|07");
        fossil_put_pipe(" PgUp/PgDn Scroll  |15ESC|07 menu  |15Up|07 recall  |15Tab|07 nick complete\r\n\r\n");
        fossil_put_pipe("|11Appearance:\r\n|07");
        fossil_put_pipe("|15/color 1-15  /prefix <txt>  /suffix <txt>  /theme 1-6\r\n|07");
        draw_hr();
        fossil_put_pipe("|08Press any key... ");

    } else if (g_help_page == 3) {
        fossil_put_pipe("|14  Server, Users & Modes\r\n\r\n|07");
        fossil_put_pipe("|11Server info:\r\n|07");
        fossil_put_pipe("|15/motd  /time  /version  /lusers  /admin  /info /links /stats\r\n");
        fossil_put_pipe("|11Switch server (mid-chat):\r\n|07");
        fossil_put_pipe("|15/connect host[:port] [+tls]|07   |08or|07 |15/server ...\r\n");
        fossil_put_pipe("|07    example: |15/connect irc.libera.chat:6697 +tls\r\n");
        fossil_put_pipe("|15/disconnect [reason]\r\n\r\n|07");
        fossil_put_pipe("|11Users:\r\n|07");
        fossil_put_pipe("|15/whois /whowas /who /ison /userhost\r\n");
        fossil_put_pipe("|15/invite nick #chan   /kick nick [reason]\r\n");
        fossil_put_pipe("|15/twit nick   /untwit nick   /twit |07(list)\r\n\r\n");
        fossil_put_pipe("|11Channel modes / ops:\r\n|07");
        fossil_put_pipe("|15/mode #chan +-flags     /umode +iw\r\n");
        fossil_put_pipe("|15/op /deop /voice /devoice nick\r\n");
        fossil_put_pipe("|15/ban /unban mask        /topic [text]\r\n");
        fossil_put_pipe("|15/raw <line>   |07Send a raw IRC line\r\n");
        draw_hr();
        fossil_put_pipe("|08Press any key... ");

    } else if (g_help_page == 4) {
        fossil_put_pipe("|14  Services, Sessions & CTCP\r\n\r\n|07");
        fossil_put_pipe("|11NickServ (the IRC form is |15/msg NickServ IDENTIFY pw|11):\r\n|07");
        fossil_put_pipe("|15/identify <pw>|07          shortcut for NickServ IDENTIFY\r\n");
        fossil_put_pipe("|15/register <pw> [email]|07  NickServ REGISTER\r\n");
        fossil_put_pipe("|15/ns <cmd>|07               Talk to NickServ\r\n");
        fossil_put_pipe("|15/cs <cmd>|07               Talk to ChanServ\r\n");
        fossil_put_pipe("|15/ms <cmd>|07               Talk to MemoServ\r\n\r\n");
        fossil_put_pipe("|11Split sessions (two servers side-by-side):\r\n|07");
        fossil_put_pipe("|15/split host[:port] [+tls] |07Open 2nd server in right pane\r\n");
        fossil_put_pipe("|15/focus |08or |15F9/F10|07       Swap focused pane\r\n");
        fossil_put_pipe("|15/unsplit                   |07Close 2nd pane\r\n\r\n");
        fossil_put_pipe("|11CTCP:|07 /ctcp nick VERSION|TIME|PING|CLIENTINFO|FINGER\r\n");
        fossil_put_pipe("|11Privacy:|07 passwords in /identify /register /oper are masked\r\n");
        draw_hr();
        fossil_put_pipe("|08Press any key... ");
    }
}

static void draw_main_menu(const dropfile_info_t *drop) {
    char colinfo[64];

    draw_header(NULL);
    draw_info_line("BBS User", drop->alias[0] ? drop->alias : drop->user_name);
    draw_info_line("Server",   "IRC server (from IRCBBS.DAT)");

    snprintf(colinfo, sizeof(colinfo), "|%02d%s%s%s|07",
             g_settings.handle_color,
             g_settings.handle_prefix,
             g_settings.nick,
             g_settings.handle_suffix);
    draw_info_line("IRC Nick", colinfo);
    draw_hr();

    draw_color_bar();
    fossil_puts("\r\n");

    fossil_put_pipe("|08  |151|07) Connect to IRC     |08(uses IRCBBS.DAT)\r\n");
    fossil_put_pipe("|08  |152|07) Choose Network     |08(Libera, Synchro, ...)\r\n");
    fossil_put_pipe("|08  |153|07) Edit Settings\r\n");
    fossil_put_pipe("|08  |154|07) Server Info\r\n");
    fossil_put_pipe("|08  |155|07) Help\r\n");
    fossil_put_pipe("|08  |156|07) Quit\r\n");
    fossil_puts("\r\n");
    draw_hr();
    fossil_put_pipe("|08Press |151-6|07: ");
}

/* Render the network picker list. */
static void draw_networks_menu(void) {
    int i;
    char buf[128];

    draw_header("|15Choose IRC Network|07");

    fossil_put_pipe("|14Pick a network to connect to:|07\r\n\r\n");

    for (i = 0; i < NUM_NETWORKS; ++i) {
        const char *mark;
        char both[32];
        if (g_networks[i].port_tls > 0 && g_networks[i].port_plain > 0) {
            snprintf(both, sizeof(both), "|10[plain %d / TLS %d]",
                     g_networks[i].port_plain, g_networks[i].port_tls);
            mark = both;
        } else if (g_networks[i].port_tls > 0) {
            snprintf(both, sizeof(both), "|10[TLS %d only]", g_networks[i].port_tls);
            mark = both;
        } else {
            snprintf(both, sizeof(both), "|08[plain %d only]", g_networks[i].port_plain);
            mark = both;
        }
        snprintf(buf, sizeof(buf), "|08 %c) |15%-17s|07 %-24s %s|07",
                 'a' + i,
                 g_networks[i].label,
                 g_networks[i].host,
                 mark);
        fossil_put_pipe(buf);
        fossil_puts("\r\n");
    }
    {
        char last[80];
        snprintf(last, sizeof(last),
                 "|08 %c) |15Manual entry...|07   |08Enter any host:port/tls\r\n",
                 'a' + NUM_NETWORKS);
        fossil_put_pipe(last);
    }

    fossil_puts("\r\n");
    draw_hr();
    fossil_put_pipe("|08Pick a letter, or |150|08/|15ESC|08 back to menu: |07");
}

/* After a network is picked that supports both plain and TLS, ask which. */
static void draw_tls_prompt(void) {
    const irc_network_t *n = &g_networks[g_net_idx];
    char buf[96];
    draw_header("|15Use TLS?|07");
    fossil_put_pipe("|07Network: |15");
    fossil_puts(n->label);
    fossil_puts(" |08("); fossil_puts(n->host); fossil_puts(")\r\n\r\n|07");
    fossil_put_pipe("|07Pick a connection mode:\r\n\r\n");
    snprintf(buf, sizeof(buf), "|08  Plain TCP:   |07port %d\r\n", n->port_plain);
    fossil_put_pipe(buf);
    snprintf(buf, sizeof(buf), "|08  TLS:         |07port %d  |10(recommended)\r\n", n->port_tls);
    fossil_put_pipe(buf);
    fossil_puts("\r\n");
    draw_hr();
    fossil_put_pipe("|08Use TLS? [|15Y|08/n] or |15ESC|08 to cancel: |07");
}

/* Manual custom host entry screens. */
static void draw_custom_host(void) {
    draw_header("|15Custom Host|07");
    fossil_put_pipe("|07Enter the IRC server hostname (e.g. |15irc.example.com|07):\r\n\r\n");
    fossil_put_pipe("|08> |07");
    fossil_put_pipe(g_input);
    fossil_putch('_');
}

static void draw_custom_port(void) {
    char buf[96];
    draw_header("|15Custom Port|07");
    snprintf(buf, sizeof(buf), "|07Host: |15%s|07\r\n\r\n", g_net_custom_host);
    fossil_put_pipe(buf);
    fossil_put_pipe("|07Enter the port (e.g. |156667|07 plain, |156697|07 TLS):\r\n\r\n");
    fossil_put_pipe("|08> |07");
    fossil_put_pipe(g_input);
    fossil_putch('_');
}

static void draw_custom_tls(void) {
    char buf[96];
    draw_header("|15Custom Connection|07");
    snprintf(buf, sizeof(buf), "|07Host: |15%s:%d|07\r\n\r\n",
             g_net_custom_host, g_net_custom_port);
    fossil_put_pipe(buf);
    fossil_put_pipe("|07Use TLS on this connection?\r\n\r\n");
    fossil_put_pipe("|08Use TLS? [y/|15N|08] or |15ESC|08 to cancel: |07");
}

static void draw_settings_menu(void) {
    char buf[128];

    draw_header("|15Edit Settings|07");
    draw_handle_preview();
    fossil_puts("\r\n");

    snprintf(buf, sizeof(buf), "%s|%02d%s%s|07",
             g_settings.handle_prefix,
             g_settings.handle_color,
             g_settings.nick,
             g_settings.handle_suffix);
    draw_info_line("|151|07) IRC Nick", buf);

    snprintf(buf, sizeof(buf), "|%02d%d|07  (use /color N in chat too)",
             g_settings.handle_color, g_settings.handle_color);
    draw_info_line("|152|07) Nick Color", buf);

    snprintf(buf, sizeof(buf), "%s", g_settings.handle_prefix[0] ? g_settings.handle_prefix : "(none)");
    draw_info_line("|153|07) Nick Prefix", buf);

    snprintf(buf, sizeof(buf), "%s", g_settings.handle_suffix[0] ? g_settings.handle_suffix : "(none)");
    draw_info_line("|154|07) Nick Suffix", buf);

    snprintf(buf, sizeof(buf), "|%02d%d|07  (chat text color)",
             g_settings.text_color, g_settings.text_color);
    draw_info_line("|155|07) Text Color", buf);

    snprintf(buf, sizeof(buf), "%s", g_settings.quitmsg);
    draw_info_line("|156|07) Quit Message", buf);

    snprintf(buf, sizeof(buf), "%s", g_settings.partmsg);
    draw_info_line("|157|07) Part Message", buf);

    draw_hr();

    if (g_edit != EDIT_NONE) {
        const char *field_names[] = {
            "", "IRC Nick", "Nick Color (1-15)", "Nick Prefix",
            "Nick Suffix", "Quit Message", "Part Message", "Text Color (1-15)"
        };
        fossil_put_pipe("|11Editing: |15");
        fossil_puts(field_names[g_edit]);
        fossil_puts("|07\r\n");

        if (g_edit == EDIT_HCOLOR || g_edit == EDIT_TEXTCOLOR)
            draw_color_bar();

        fossil_put_pipe("|08New value (Enter=save, Esc=cancel): |07");
        fossil_put_pipe(g_input);
        fossil_putch('_');
    } else {
        fossil_put_pipe("|08[1-7] Edit field    [B] Back to menu|07\r\n");
        fossil_put_pipe("|08> ");
    }
}

static void draw_stats(const char *raw) {
    int users = 0, chans = 0, servers = 0, opers = 0;
    char server_name[128];
    server_name[0] = 0;

    draw_header("|15IRC Server Info|07");

    sscanf(raw ? raw : "0 0 0 0 ", "%d %d %d %d %127[^\n]",
           &users, &chans, &opers, &servers, server_name);

    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", users);
        draw_info_line("Users Online   ", buf);
        snprintf(buf, sizeof(buf), "%d", chans);
        draw_info_line("Channels       ", buf);
        snprintf(buf, sizeof(buf), "%d", opers);
        draw_info_line("IRC Operators  ", buf);
        snprintf(buf, sizeof(buf), "%d", servers);
        draw_info_line("Servers Linked ", buf);
        draw_info_line("Server         ", server_name[0] ? server_name : "(unknown)");
    }

    draw_hr();
    fossil_put_pipe("|08Server: |07IRC server (from IRCBBS.DAT)\r\n");
    fossil_puts("\r\n");
    fossil_put_pipe("|15Press any key to return...|07");
}

static void draw_stats_waiting(void) {
    draw_header("|15IRC Server Info|07");
    fossil_put_pipe("|14Connecting to server for info...|07\r\n");
    fossil_puts("Please wait.\r\n");
}

static const char *ci_strstr(const char *hay, const char *needle) {
    int nlen = (int)strlen(needle);
    if (!nlen) return hay;
    for (; *hay; ++hay) {
        if (strnicmp(hay, needle, (size_t)nlen) == 0)
            return hay;
    }
    return NULL;
}

static void local_time_str(char *buf, int bufsz) {
    time_t t = time(NULL);
    struct tm *tm;
    if (t == (time_t)-1) { safe_copy(buf, (size_t)bufsz, "??:??"); return; }
    tm = localtime(&t);
    if (!tm) { safe_copy(buf, (size_t)bufsz, "??:??"); return; }
    snprintf(buf, (size_t)bufsz, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

static void draw_chat_info(void) {
    char buf[SCREEN_W + 64];

    ansi_goto(1, 1);
    fossil_puts("\033[K");
    snprintf(buf, sizeof(buf), "|11Chan:|07 %-14s |13Topic:|07 %s",
             g_chat_chan[0]  ? g_chat_chan  : "---",
             g_chat_topic[0] ? g_chat_topic : "(no topic)");
    fossil_put_pipe(buf);

    {
        char cnt_part[24];
        if (g_input_len >= 400)
            snprintf(cnt_part, sizeof(cnt_part), "|12%d|07/400", g_input_len);
        else if (g_input_len >= 380)
            snprintf(cnt_part, sizeof(cnt_part), "|14%d|07/400", g_input_len);
        else
            snprintf(cnt_part, sizeof(cnt_part), "|07%d|07/400", g_input_len);

        ansi_goto(2, 1);
        fossil_puts("\033[K");
        snprintf(buf, sizeof(buf),
                 "|10Status:|07 %-11s |14Users:|07 %-5s |08Len:|07 %s",
                 g_chat_status[0] ? g_chat_status : "IDLE",
                 g_chat_users[0]  ? g_chat_users  : "?",
                 cnt_part);
        fossil_put_pipe(buf);
    }
}

static void draw_char_count_only(void) {
    char cnt_buf[20];
    if (g_input_len >= 400)
        snprintf(cnt_buf, sizeof(cnt_buf), "|12%d|07/400", g_input_len);
    else if (g_input_len >= 380)
        snprintf(cnt_buf, sizeof(cnt_buf), "|14%d|07/400", g_input_len);
    else
        snprintf(cnt_buf, sizeof(cnt_buf), "|07%d|07/400", g_input_len);
    /* Col 39 = just past "Status: <11> Users: <5> Len: ". */
    ansi_goto(2, 39);
    fossil_puts("\033[K");
    fossil_put_pipe(cnt_buf);
}

static void draw_chat_header(void) {
    fossil_cls();
    draw_chat_info();
    ansi_goto(3, 1);
    draw_hr();
}

static void draw_chat_msgs(void) {
    int row, j;
    int end, first;
    char prompt[64];

    end = g_msg_count - g_scroll_off;
    if (end < 0) end = 0;
    first = (end > CHAT_MSG_ROWS) ? end - CHAT_MSG_ROWS : 0;

    fossil_puts("\033[?25l");

    for (row = CHAT_MSG_TOP; row <= CHAT_MSG_BOT; ++row) {
        int mi = first + (row - CHAT_MSG_TOP);
        ansi_goto(row, 1);
        fossil_puts("\033[K");
        if (mi < end && mi < g_msg_count)
            fossil_put_pipe(g_msgs[mi]);
    }

    {
        int t2; static const char *ta[7] = {
            "\033[90m","\033[90m","\033[34m","\033[32m",
            "\033[36m","\033[31m","\033[35m"
        };
        t2 = g_settings.theme; if (t2 < 1 || t2 > 6) t2 = 1;
        ansi_goto(INPUT_RULE_ROW, 1);
        fossil_puts("\033[K");
        fossil_puts(ta[t2]);
        for (j = 0; j < SCREEN_W; ++j)
            fossil_putch(0xDC);
        fossil_puts("\033[0m");
    }
    if (g_scroll_off > 0) {
        char scr_buf[64];
        ansi_goto(INPUT_RULE_ROW, 1);
        if (g_new_while_scrolled > 0)
            snprintf(scr_buf, sizeof(scr_buf),
                     "|14[ SCROLL: UP/DN - %d new - ESC to resume ]|07", g_new_while_scrolled);
        else
            safe_copy(scr_buf, sizeof(scr_buf), "|14[ SCROLL: UP/DN - ESC to resume ]|07");
        fossil_put_pipe(scr_buf);
    }

    ansi_goto(INPUT_PROMPT_ROW, 1);
    fossil_puts("\033[K");
    snprintf(prompt, sizeof(prompt), "%s|%02d%s%s|07 ",
             g_settings.handle_prefix,
             g_settings.handle_color,
             g_settings.nick,
             g_settings.handle_suffix);
    fossil_put_pipe(prompt);
    put_input_colored();

    fossil_puts("\033[?25h");
}

static void draw_chat(void) {
    draw_chat_header();
    draw_chat_msgs();
}

/* Split the tab-users CSV into an array of nicknames (pointers into src). */
static int split_users(char *src, char *nicks[], int max) {
    int count = 0;
    char *p = src;
    while (*p && count < max) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        nicks[count++] = p;
        while (*p && *p != ',') ++p;
        if (*p) { *p = 0; ++p; }
    }
    return count;
}

/* Show a paginated popup of current channel users. Blocks until ENTER/ESC. */
static void show_users_pane(void) {
    char users_copy[sizeof(g_tab_users)];
    char *nicks[256];
    int count, pages, page = 0;
    const int PER_ROW = 4;
    int rows_per_page = CHAT_MSG_ROWS;
    int per_page;

    if (!g_tab_users[0]) {
        add_msg("|07No users tracked (not in a channel yet, or list not received).");
        draw_chat_info();
        draw_chat_msgs();
        redraw_chat_input();
        return;
    }

    safe_copy(users_copy, sizeof(users_copy), g_tab_users);
    count = split_users(users_copy, nicks, 256);
    if (count == 0) {
        add_msg("|07User list is empty.");
        draw_chat_info();
        draw_chat_msgs();
        redraw_chat_input();
        return;
    }

    per_page = rows_per_page * PER_ROW;
    pages = (count + per_page - 1) / per_page;

    for (;;) {
        int start = page * per_page;
        int end   = start + per_page;
        int row   = CHAT_MSG_TOP;
        int done  = 0;
        int i, col;

        if (end > count) end = count;

        fossil_cls();
        ansi_goto(1, 1);
        {
            char hdr[80];
            snprintf(hdr, sizeof(hdr), "|14USERS IN %s|07",
                     g_chat_chan[0] ? g_chat_chan : "(no channel)");
            fossil_put_pipe(hdr);
        }
        ansi_goto(2, 1);
        {
            char hdr[80];
            snprintf(hdr, sizeof(hdr),
                     "|08Page |15%d|08/|15%d  |08total |15%d|08 nicks|07",
                     page + 1, pages, count);
            fossil_put_pipe(hdr);
        }
        ansi_goto(3, 1);
        draw_hr();

        /* Render nicks as a grid, 4 columns x 19 rows = 76 per page. */
        col = 0;
        ansi_goto(row, 1);
        fossil_puts("\033[K");
        for (i = start; i < end; ++i) {
            char cell[32];
            const char *n = nicks[i];
            /* Strip mode prefix for display color; keep prefix visible. */
            int hilite_color = 7;
            if (*n == '@') hilite_color = 14;       /* op */
            else if (*n == '%') hilite_color = 10;  /* halfop */
            else if (*n == '+') hilite_color = 11;  /* voice */
            else if (*n == '~' || *n == '&') hilite_color = 12; /* owner/admin */
            snprintf(cell, sizeof(cell), "|%02d%-18s|07", hilite_color, n);
            fossil_put_pipe(cell);
            col++;
            if (col >= PER_ROW) {
                col = 0;
                row++;
                if (row > CHAT_MSG_BOT) break;
                ansi_goto(row, 1);
                fossil_puts("\033[K");
            }
        }

        ansi_goto(INPUT_RULE_ROW, 1);
        fossil_puts("\033[K");
        fossil_put_pipe(
            "|14[ PgUp/PgDn/Home/End - ENTER or ESC to return to chat ]|07");
        ansi_goto(INPUT_PROMPT_ROW, 1);
        fossil_puts("\033[K");

        while (!done) {
            int ch = fossil_getch_nonblock();
            if (ch < 0) { delay(20); continue; }

            if (ch == 13 || ch == 10) { done = 2; break; }

            if (ch == 27) {
                int ch2 = -1, ch3 = -1, r;
                for (r = 0; r < 25; ++r) {
                    ch2 = fossil_getch_nonblock();
                    if (ch2 >= 0) break;
                    delay(2);
                }
                if (ch2 != '[' && ch2 != 'O') { done = 2; break; }
                for (r = 0; r < 25; ++r) {
                    ch3 = fossil_getch_nonblock();
                    if (ch3 >= 0) break;
                    delay(2);
                }
                if (ch3 == '5' || ch3 == 'I') {
                    if (ch3 == '5') (void)fossil_getch_nonblock();
                    if (page > 0) { --page; done = 1; }
                } else if (ch3 == '6' || ch3 == 'G') {
                    if (ch3 == '6') (void)fossil_getch_nonblock();
                    if (page < pages - 1) { ++page; done = 1; }
                } else if (ch3 == 'H' || ch3 == '1') {
                    if (ch3 == '1') (void)fossil_getch_nonblock();
                    if (page != 0) { page = 0; done = 1; }
                } else if (ch3 == 'F' || ch3 == '4') {
                    if (ch3 == '4') (void)fossil_getch_nonblock();
                    if (page != pages - 1) { page = pages - 1; done = 1; }
                }
                continue;
            }

            if (ch == 0) {
                int scan = fossil_getch_nonblock();
                if (scan < 0) continue;
                if (scan == 73 && page > 0) { --page; done = 1; }
                else if (scan == 81 && page < pages - 1) { ++page; done = 1; }
                else if (scan == 71 && page != 0) { page = 0; done = 1; }
                else if (scan == 79 && page != pages - 1) {
                    page = pages - 1; done = 1;
                }
            }
        }
        if (done == 2) break;
    }

    draw_chat();
}

static int pipe_vis_len(const char *s) {
    int n = 0;
    while (*s) {
        if (*s == '|' && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2])) {
            s += 3; continue;
        }
        ++n; ++s;
    }
    return n;
}

static int chat_cursor_col(void) {
    char prompt[64];
    int vis;
    snprintf(prompt, sizeof(prompt), "%s|%02d%s%s|07 ",
             g_settings.handle_prefix, g_settings.handle_color,
             g_settings.nick, g_settings.handle_suffix);
    vis = g_input_len < g_chat_view_w ? g_input_len : g_chat_view_w;
    return pipe_vis_len(prompt) + vis + 1;
}

/* Mask passwords in /identify, /register, /ns identify, /cs register, /msg NickServ IDENTIFY */
static int masked_range(int *ms, int *me) {
    int i;
    *ms = -1; *me = -1;
    if (strncmp(g_input, "/identify ", 10) == 0) {
        *ms = 10; *me = g_input_len;
        return 1;
    }
    if (strncmp(g_input, "/register ", 10) == 0) {
        *ms = 10; *me = g_input_len;
        for (i = 10; i < g_input_len; ++i)
            if (g_input[i] == ' ') { *me = i; break; }
        return 1;
    }
    if (strncmp(g_input, "/oper ", 6) == 0) {
        int s2 = -1;
        for (i = 6; i < g_input_len; ++i)
            if (g_input[i] == ' ') { s2 = i + 1; break; }
        if (s2 > 0) { *ms = s2; *me = g_input_len; return 1; }
    }
    return 0;
}

static void put_input_colored(void) {
    int i, view_start, ms, me;
    view_start = (g_input_len > g_chat_view_w) ? (g_input_len - g_chat_view_w) : 0;
    masked_range(&ms, &me);

    ansi_color_code(g_settings.text_color);
    if (g_tab_hl_start >= 0 && g_tab_hl_end > g_tab_hl_start
            && g_tab_hl_end <= g_input_len) {
        int hl_s = g_tab_hl_start > view_start ? g_tab_hl_start : view_start;
        for (i = view_start; i < hl_s; ++i)
            fossil_putch((i >= ms && i < me) ? '*' : (unsigned char)g_input[i]);
        fossil_puts("\033[95m");
        for (i = hl_s; i < g_tab_hl_end; ++i)
            fossil_putch((i >= ms && i < me) ? '*' : (unsigned char)g_input[i]);
        ansi_color_code(g_settings.text_color);
        for (i = g_tab_hl_end; i < g_input_len; ++i)
            fossil_putch((i >= ms && i < me) ? '*' : (unsigned char)g_input[i]);
    } else {
        for (i = view_start; i < g_input_len; ++i)
            fossil_putch((i >= ms && i < me) ? '*' : (unsigned char)g_input[i]);
    }
}

static void redraw_chat_input(void) {
    char prompt[64];
    ansi_goto(INPUT_PROMPT_ROW, 1);
    fossil_puts("\033[K");
    snprintf(prompt, sizeof(prompt), "%s|%02d%s%s|07 ",
             g_settings.handle_prefix,
             g_settings.handle_color,
             g_settings.nick,
             g_settings.handle_suffix);
    fossil_put_pipe(prompt);
    put_input_colored();
}

/* ============================================================================
   HELPER SYNC
   ============================================================================ */

static void sync_settings_to_helper(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "SET HANDLECOLOR %d", g_settings.handle_color);
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET PREFIX %s",
             g_settings.handle_prefix[0] ? g_settings.handle_prefix : "NONE");
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET SUFFIX %s",
             g_settings.handle_suffix[0] ? g_settings.handle_suffix : "NONE");
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET QUITMSG %s", g_settings.quitmsg);
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET PARTMSG %s", g_settings.partmsg);
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET TEXTCOLOR %d", g_settings.text_color);
    bridge_send_line(cmd);
}

/* Tell the bridge to use whatever IRCBBS.DAT specifies (sentinel "(cfg)"). */
static void send_connect(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "CONNECT (cfg) 0 0 %s", g_settings.nick);
    bridge_send_line(cmd);
    sync_settings_to_helper();
}

/* Tell the bridge to connect to an explicit host:port, with TLS on/off. */
static void send_connect_explicit(const char *host, int port, int tls) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "CONNECT %s %d %d %s",
             host, port, tls, g_settings.nick);
    bridge_send_line(cmd);
    sync_settings_to_helper();
}

static void send_quit_once(void) {
    if (!g_sent_quit) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "QUIT %s",
                 g_settings.quitmsg[0] ? g_settings.quitmsg : "Leaving");
        bridge_send_line(cmd);
        g_sent_quit = 1;
    }
}

/* ============================================================================
   INPUT VALIDATION HELPERS
   ============================================================================ */

/* Sanitize an IRC nick: only letters/digits/_-`[]{}\\^|; max 20 chars. */
static void sanitize_nick(char *out, size_t outsz, const char *src) {
    size_t i = 0;
    while (*src && i + 1 < outsz && i < 20) {
        unsigned char c = (unsigned char)*src;
        if (c == '|' && isdigit((unsigned char)src[1]) && isdigit((unsigned char)src[2])) {
            src += 3; continue;
        }
        /* RFC 2812 allows letter digit special = %x5B-60 / %x7B-7D / - */
        if (isalnum(c) || c == '-' || c == '_' || c == '[' || c == ']' ||
            c == '{' || c == '}' || c == '\\' || c == '^' || c == '`') {
            if (i == 0 && !isalpha(c)) { src++; continue; }
            out[i++] = (char)c;
        }
        src++;
    }
    out[i] = 0;
}

static void sanitize_field(char *out, size_t outsz, const char *src, size_t maxlen) {
    size_t i = 0;
    if (maxlen > outsz - 1) maxlen = outsz - 1;
    while (*src && i < maxlen) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32) continue;
        out[i++] = (char)c;
    }
    out[i] = 0;
}

/* ============================================================================
   CHAT SLASH COMMAND HANDLER
   ============================================================================ */

static void handle_slash_command(const char *input_orig) {
    char slash_buf[512];
    const char *input;
    char cmd[512];
    const char *args, *sp;
    {
        int i = 0;
        while (i < (int)sizeof(slash_buf) - 1 && input_orig[i] && input_orig[i] != ' ')
            slash_buf[i] = (char)tolower((unsigned char)input_orig[i]), i++;
        while (i < (int)sizeof(slash_buf) - 1 && input_orig[i])
            slash_buf[i] = input_orig[i], i++;
        slash_buf[i] = 0;
        input = slash_buf;
    }

    if (strcmp(input, "/help") == 0) {
        add_msg("|14=== ANETIRC Client Commands ===");
        add_msg("|11Channels:");
        add_msg("|15/join #chan [key]  /j #chan   |07Join channel");
        add_msg("|15/part [#chan]      /leave     |07Leave channel");
        add_msg("|15/hop               /cycle     |07Part and rejoin");
        add_msg("|15/win #chan                    |07Switch active channel");
        add_msg("|15/list [pat]        /names     |07Channel/user listings");
        add_msg("|15/topic [text]                 |07Get/set topic");
        add_msg("|15/users |08or|15 /u                   |07Popup: users in channel");
        add_msg("|11Messaging:");
        add_msg("|15/msg <nick> <txt>  /t <n> <x> |07Private message");
        add_msg("|15/query <nick>                 |07Open PM tracker for nick");
        add_msg("|15/notice <nick> <x>            |07Send a NOTICE");
        add_msg("|15/r <text>                     |07Reply to last PM");
        add_msg("|15/me <text>                    |07CTCP ACTION");
        add_msg("|15/ctcp <nick> CMD              |07CTCP (VERSION/TIME/PING)");
        add_msg("|11Users & Mode:");
        add_msg("|15/whois /whowas /who /ison /userhost");
        add_msg("|15/nick <newnick>   /away [msg] |15/back");
        add_msg("|15/umode +iw       /mode #chan flags");
        add_msg("|15/op /deop /voice /devoice /ban /unban /kick /invite");
        add_msg("|11Services (real IRC form: |15/msg NickServ IDENTIFY pw|11):");
        add_msg("|15/identify <pw>                |07NickServ IDENTIFY shortcut");
        add_msg("|15/register <pw> [email]");
        add_msg("|15/ns CMD   /cs CMD   /ms CMD");
        add_msg("|11Server:");
        add_msg("|15/motd /time /version /lusers /info /stats /links /admin");
        add_msg("|15/connect host[:port] [+tls]   |07Switch IRC server");
        add_msg("|15/server ... |07(alias)         |15/disconnect [reason]");
        add_msg("|15/raw <line>                   |07Send raw IRC line");
        add_msg("|11Sessions (HexChat-style split):");
        add_msg("|15/split host[:port] [+tls]     |07Dual-server mode");
        add_msg("|15/focus     F9 / F10           |07Swap focus pane");
        add_msg("|15/unsplit                      |07Close 2nd pane");
        add_msg("|11Client:");
        add_msg("|15/twit /untwit      |15/clear    |07Ignore list / clear scroll");
        add_msg("|15/color /prefix /suffix /theme");
        add_msg("|15/quit [reason]");
        draw_chat_msgs();
        return;
    }

    if (strncmp(input, "/quit", 5) == 0) {
        const char *a = input + 5;
        if (*a == ' ') ++a;
        if (*a) {
            snprintf(cmd, sizeof(cmd), "QUIT %s", a);
            bridge_send_line(cmd);
            g_sent_quit = 1;
        }
        g_return_to_menu = 1;
        return;
    }

    if (strcmp(input, "/users") == 0 || strcmp(input, "/u") == 0) {
        /* Ask bridge for a fresh names list, then show the popup. */
        bridge_send_line("NAMES");
        show_users_pane();
        return;
    }

    if (strcmp(input, "/clear") == 0) {
        g_msg_count = 0;
        g_scroll_off = 0;
        g_new_while_scrolled = 0;
        draw_chat_msgs();
        return;
    }

    if (strncmp(input, "/connect", 8) == 0 || strncmp(input, "/server", 7) == 0) {
        int off = (input[1] == 'c') ? 8 : 7;
        const char *a = input + off;
        while (*a == ' ') ++a;
        if (!*a) { add_msg("|12ERR|07 Usage: /connect host[:port] [+tls]"); return; }
        snprintf(cmd, sizeof(cmd), "SWITCH %s", a);
        bridge_send_line(cmd);
        return;
    }
    if (strncmp(input, "/disconnect", 11) == 0) {
        const char *a = input + 11;
        if (*a == ' ') ++a;
        if (*a) snprintf(cmd, sizeof(cmd), "DISCONNECT %s", a);
        else    snprintf(cmd, sizeof(cmd), "DISCONNECT");
        bridge_send_line(cmd);
        return;
    }

    if (strncmp(input, "/split", 6) == 0) {
        const char *a = input + 6;
        while (*a == ' ') ++a;
        if (!*a) { add_msg("|12ERR|07 Usage: /split host[:port] [+tls]"); return; }
        snprintf(cmd, sizeof(cmd), "SPLIT %s", a);
        bridge_send_line(cmd);
        return;
    }
    if (strcmp(input, "/unsplit") == 0) {
        bridge_send_line("UNSPLIT");
        return;
    }
    if (strcmp(input, "/focus") == 0) {
        bridge_send_line("FOCUS");
        return;
    }

    if (strncmp(input, "/join ", 6) == 0 || strncmp(input, "/j ", 3) == 0) {
        int off = (input[2] == ' ') ? 3 : 6;
        snprintf(cmd, sizeof(cmd),"JOIN %s", input + off);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/part", 5) == 0 || strncmp(input, "/leave", 6) == 0) {
        int off = (input[1] == 'p') ? 5 : 6;
        const char *a = input + off;
        if (*a == ' ') ++a;
        snprintf(cmd, sizeof(cmd), "PART %s", a);
        bridge_send_line(cmd); return;
    }
    if (strcmp(input, "/hop") == 0 || strcmp(input, "/cycle") == 0) {
        bridge_send_line("HOP");
        return;
    }

    if (strncmp(input, "/win ", 5) == 0) {
        snprintf(cmd, sizeof(cmd), "SETCHAN %s", input + 5);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/query ", 7) == 0) {
        const char *n = input + 7;
        safe_copy(g_last_dm_target, sizeof(g_last_dm_target), n);
        { char m[80]; snprintf(m, sizeof(m), "|11QUERY|07 PM target set to |11%s|07. Use |15/r|07 or |15/msg %s|07 to send.", n, n); add_msg(m); }
        return;
    }

    if (strncmp(input, "/identify ", 10) == 0) {
        snprintf(cmd, sizeof(cmd),"IDENTIFY %s", input + 10);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/register ", 10) == 0) {
        snprintf(cmd, sizeof(cmd),"REGISTER %s", input + 10);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/ns ", 4) == 0 || strncmp(input, "/nickserv ", 10) == 0) {
        int off = (input[2] == ' ') ? 3 : 9;
        snprintf(cmd, sizeof(cmd), "NICKSERV %s", input + off + 1);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/cs ", 4) == 0 || strncmp(input, "/chanserv ", 10) == 0) {
        int off = (input[2] == ' ') ? 3 : 9;
        snprintf(cmd, sizeof(cmd), "CHANSERV %s", input + off + 1);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/ms ", 4) == 0 || strncmp(input, "/memoserv ", 10) == 0) {
        int off = (input[2] == ' ') ? 3 : 9;
        snprintf(cmd, sizeof(cmd), "MEMOSERV %s", input + off + 1);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/msg ", 5) == 0 || strncmp(input, "/t ", 3) == 0) {
        int off = (input[2] == ' ') ? 3 : 5;
        args = input + off;
        sp = strchr(args, ' ');
        if (sp && sp > args) {
            int ulen = (int)(sp - args);
            if (ulen > 0 && ulen < (int)sizeof(g_last_dm_target)) {
                memcpy(g_last_dm_target, args, (size_t)ulen);
                g_last_dm_target[ulen] = 0;
            }
            snprintf(cmd, sizeof(cmd),"MSG %.*s %s", ulen, args, sp + 1);
            bridge_send_line(cmd);
        } else { add_msg("|12ERR|07 Usage: /msg nick text  (or /t nick text)"); }
        return;
    }

    if (strncmp(input, "/notice ", 8) == 0) {
        snprintf(cmd, sizeof(cmd), "NOTICE %s", input + 8);
        bridge_send_line(cmd);
        return;
    }

    if (strncmp(input, "/r ", 3) == 0) {
        if (!g_last_dm_target[0]) {
            add_msg("|12ERR|07 No previous DM. Use /msg <nick> <text> first.");
            return;
        }
        snprintf(cmd, sizeof(cmd), "MSG %s %s", g_last_dm_target, input + 3);
        bridge_send_line(cmd);
        return;
    }

    if (strncmp(input, "/me ", 4) == 0) {
        snprintf(cmd, sizeof(cmd),"ME %s", input + 4);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/ctcp ", 6) == 0) {
        snprintf(cmd, sizeof(cmd),"CTCP %s", input + 6);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/nick ", 6) == 0) {
        snprintf(cmd, sizeof(cmd),"NICK %s", input + 6);
        bridge_send_line(cmd);
        /* Update local nick immediately; bridge will confirm via NICK echo. */
        sanitize_nick(g_settings.nick, sizeof(g_settings.nick), input + 6);
        save_settings(g_bbs_user, &g_settings);
        return;
    }

    if (strcmp(input, "/names") == 0) { bridge_send_line("NAMES"); return; }
    if (strncmp(input, "/names ", 7) == 0) {
        snprintf(cmd, sizeof(cmd), "NAMES %s", input + 7);
        bridge_send_line(cmd); return;
    }
    if (strcmp(input, "/who") == 0)   { bridge_send_line("WHO"); return; }
    if (strncmp(input, "/who ", 5) == 0) {
        snprintf(cmd, sizeof(cmd), "WHO %s", input + 5);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/whois ", 7) == 0) {
        snprintf(cmd, sizeof(cmd), "WHOIS %s", input + 7);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/whowas ", 8) == 0) {
        snprintf(cmd, sizeof(cmd), "WHOWAS %s", input + 8);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/ison ", 6) == 0) {
        snprintf(cmd, sizeof(cmd), "ISON %s", input + 6);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/userhost ", 10) == 0) {
        snprintf(cmd, sizeof(cmd), "USERHOST %s", input + 10);
        bridge_send_line(cmd); return;
    }
    if (strcmp(input, "/list")    == 0) { bridge_send_line("LIST");    return; }
    if (strncmp(input, "/list ", 6) == 0) {
        snprintf(cmd, sizeof(cmd), "LIST %s", input + 6);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/topic", 6) == 0) {
        const char *a = input + 6;
        if (*a == ' ') ++a;
        snprintf(cmd, sizeof(cmd), "TOPIC %s", a);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/mode ", 6) == 0) {
        snprintf(cmd, sizeof(cmd), "MODE %s", input + 6);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/umode ", 7) == 0) {
        snprintf(cmd, sizeof(cmd), "UMODE %s", input + 7);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/op ", 4) == 0) {
        snprintf(cmd, sizeof(cmd), "MODESHORT op %s", input + 4); bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/deop ", 6) == 0) {
        snprintf(cmd, sizeof(cmd), "MODESHORT deop %s", input + 6); bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/voice ", 7) == 0) {
        snprintf(cmd, sizeof(cmd), "MODESHORT voice %s", input + 7); bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/devoice ", 9) == 0) {
        snprintf(cmd, sizeof(cmd), "MODESHORT devoice %s", input + 9); bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/ban ", 5) == 0) {
        snprintf(cmd, sizeof(cmd), "MODESHORT ban %s", input + 5); bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/unban ", 7) == 0) {
        snprintf(cmd, sizeof(cmd), "MODESHORT unban %s", input + 7); bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/kick ", 6) == 0) {
        snprintf(cmd, sizeof(cmd), "KICK %s", input + 6);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/invite ", 8) == 0) {
        snprintf(cmd, sizeof(cmd), "INVITE %s", input + 8);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/away", 5) == 0) {
        const char *a = input + 5;
        if (*a == ' ') ++a;
        if (*a) snprintf(cmd, sizeof(cmd),"AWAY %s", a);
        else    snprintf(cmd, sizeof(cmd),"AWAY");
        bridge_send_line(cmd); return;
    }
    if (strcmp(input, "/back") == 0) {
        bridge_send_line("BACK");
        add_msg("|10BACK|07 AFK cleared. Welcome back!");
        return;
    }

    if (strcmp(input, "/motd")    == 0) { bridge_send_line("MOTD");    return; }
    if (strcmp(input, "/time")    == 0) { bridge_send_line("TIME");    return; }
    if (strcmp(input, "/version") == 0) { bridge_send_line("VERSION"); return; }
    if (strcmp(input, "/lusers")  == 0) { bridge_send_line("LUSERS");  return; }
    if (strcmp(input, "/admin")   == 0) { bridge_send_line("ADMIN");   return; }
    if (strcmp(input, "/info")    == 0) { bridge_send_line("INFO");    return; }
    if (strcmp(input, "/stats")   == 0) { bridge_send_line("STATS");   return; }
    if (strcmp(input, "/links")   == 0) { bridge_send_line("LINKS");   return; }

    if (strncmp(input, "/raw ", 5) == 0) {
        snprintf(cmd, sizeof(cmd), "RAW %s", input + 5);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/oper ", 6) == 0) {
        snprintf(cmd, sizeof(cmd), "OPER %s", input + 6);
        bridge_send_line(cmd); return;
    }

    /* Scroll helpers */
    if (strcmp(input, "/scroll") == 0 ||
        strncmp(input, "/scroll up", 10) == 0) {
        int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
        if (max_scroll > 0) {
            g_scroll_off++;
            if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
            draw_chat_msgs();
        }
        return;
    }
    if (strcmp(input, "/pgup") == 0) {
        int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
        g_scroll_off += CHAT_MSG_ROWS;
        if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
        draw_chat_msgs();
        return;
    }
    if (strncmp(input, "/scroll down", 12) == 0 ||
        strcmp(input, "/scroll live") == 0) {
        if (g_scroll_off > 0) {
            g_scroll_off--;
            if (g_scroll_off == 0) g_new_while_scrolled = 0;
            draw_chat_msgs();
        }
        return;
    }
    if (strcmp(input, "/pgdn") == 0) {
        g_scroll_off -= CHAT_MSG_ROWS;
        if (g_scroll_off < 0) g_scroll_off = 0;
        if (g_scroll_off == 0) g_new_while_scrolled = 0;
        draw_chat_msgs();
        return;
    }

    /* Twit (ignore) list */
    if (strncmp(input, "/twit", 5) == 0) {
        const char *a = input + 5;
        if (*a == ' ') ++a;
        if (*a) {
            int r = twit_add(a);
            if (r == 1) {
                char m[80];
                snprintf(m, sizeof(m), "|11TWIT|07 |12%s|07 added to ignore list.", a);
                add_msg(m);
            } else if (r == 0) {
                add_msg("|11TWIT|07 Already on ignore list.");
            } else {
                add_msg("|12ERR|07 Ignore list full (max 10).");
            }
        } else {
            if (g_settings.twit_count == 0) {
                add_msg("|11TWIT|07 Ignore list is empty.");
            } else {
                char m[200];
                int ti;
                add_msg("|11TWIT|07 Ignored nicks:");
                for (ti = 0; ti < g_settings.twit_count; ti++) {
                    snprintf(m, sizeof(m), "|11  %d|07) |12%s|07",
                             ti + 1, g_settings.twit_list[ti]);
                    add_msg(m);
                }
            }
        }
        draw_chat_msgs();
        return;
    }

    if (strncmp(input, "/untwit ", 8) == 0) {
        const char *a = input + 8;
        if (twit_remove(a)) {
            char m[80];
            snprintf(m, sizeof(m), "|11TWIT|07 |14%s|07 removed from ignore list.", a);
            add_msg(m);
        } else {
            add_msg("|12ERR|07 Nick not in ignore list.");
        }
        draw_chat_msgs();
        return;
    }

    if (strncmp(input, "/color ", 7) == 0) {
        int c = atoi(input + 7);
        if (c < 1 || c > 15) { add_msg("|12ERR|07 Color 1-15"); return; }
        g_settings.handle_color = c;
        save_settings(g_bbs_user, &g_settings);
        snprintf(cmd, sizeof(cmd),"SET HANDLECOLOR %d", c);
        bridge_send_line(cmd);
        { char m[64]; snprintf(m, sizeof(m), "|11COLOR|07 Nick color set: |%02d%d|07", c, c); add_msg(m); }
        return;
    }

    if (strncmp(input, "/prefix", 7) == 0) {
        const char *val = (input[7] == ' ') ? input + 8 : "";
        sanitize_field(g_settings.handle_prefix, sizeof(g_settings.handle_prefix), val, 30);
        save_settings(g_bbs_user, &g_settings);
        snprintf(cmd, sizeof(cmd),"SET PREFIX %s",
                g_settings.handle_prefix[0] ? g_settings.handle_prefix : "NONE");
        bridge_send_line(cmd);
        add_msg("|11PREFIX|07 Updated");
        return;
    }

    if (strncmp(input, "/suffix", 7) == 0) {
        const char *val = (input[7] == ' ') ? input + 8 : "";
        sanitize_field(g_settings.handle_suffix, sizeof(g_settings.handle_suffix), val, 30);
        save_settings(g_bbs_user, &g_settings);
        snprintf(cmd, sizeof(cmd),"SET SUFFIX %s",
                g_settings.handle_suffix[0] ? g_settings.handle_suffix : "NONE");
        bridge_send_line(cmd);
        add_msg("|11SUFFIX|07 Updated");
        return;
    }

    if (strncmp(input, "/theme ", 7) == 0) {
        int t = atoi(input + 7);
        if (t < 1 || t > 6) {
            add_msg("|12ERR|07 Theme 1-6:  1=gray 2=blue 3=green 4=cyan 5=red 6=magenta");
            return;
        }
        g_settings.theme = t;
        save_settings(g_bbs_user, &g_settings);
        draw_chat_header();
        { char m[64]; snprintf(m, sizeof(m), "|11THEME|07 Changed to %d", t); add_msg(m); }
        draw_chat_msgs();
        return;
    }

    { char m[128]; snprintf(m, sizeof(m), "|12ERR|07 Unknown: %s (try /help)", input); add_msg(m); }
}

/* ============================================================================
   MENTION DETECTION
   ============================================================================ */

static int is_mentionable_chat_line(const char *line) {
    if (!line || !line[0]) return 0;
    if (strncmp(line, "|08*",       4)  == 0) return 0;
    if (strncmp(line, "|08(",       4)  == 0) return 0;
    if (strncmp(line, "|08[|14CTCP",10) == 0) return 0;
    if (strncmp(line, "|08[|14CTCP-REPLY",16) == 0) return 0;
    if (strncmp(line, "|10SERVER",  9)  == 0) return 0;
    if (strncmp(line, "|10READY",   8)  == 0) return 0;
    if (strncmp(line, "|14NOTICE",  9)  == 0) return 0;
    if (strncmp(line, "|11JOIN",    7)  == 0) return 0;
    if (strncmp(line, "|11PART",    7)  == 0) return 0;
    if (strncmp(line, "|11NICK",    7)  == 0) return 0;
    if (strncmp(line, "|11MODE",    7)  == 0) return 0;
    if (strncmp(line, "|11TOPIC",   8)  == 0) return 0;
    if (strncmp(line, "|11KICK",    7)  == 0) return 0;
    if (strncmp(line, "|11QUIT",    7)  == 0) return 0;
    if (strncmp(line, "|12",        3)  == 0) return 0;
    if (strncmp(line, "|13(",       4)  == 0) return 0;
    if (strncmp(line, "|13* ",      5)  == 0) return 0;
    if (strncmp(line, "Connecting", 10) == 0) return 0;
    {
        char own[128];
        snprintf(own, sizeof(own), "%s|%02d%s%s|07",
                 g_settings.handle_prefix,
                 g_settings.handle_color,
                 g_settings.nick,
                 g_settings.handle_suffix);
        if (strncmp(line, own, strlen(own)) == 0) return 0;
    }
    return 1;
}

static void strip_pipe_for_mention(char *dst, int dstsz, const char *src) {
    int i = 0;
    while (*src && i < dstsz - 1) {
        if (src[0] == '|' &&
            (unsigned char)src[1] >= '0' && (unsigned char)src[1] <= '9' &&
            (unsigned char)src[2] >= '0' && (unsigned char)src[2] <= '9') {
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = 0;
}

static int nick_mentioned(const char *hay, const char *nick) {
    char stripped[MSG_LEN + 16];
    int hlen = (int)strlen(nick);
    const char *p;
    if (!hlen) return 0;
    strip_pipe_for_mention(stripped, sizeof(stripped), hay);
    p = stripped;
    while ((p = ci_strstr(p, nick)) != NULL) {
        int before = (p == stripped) || (!isalnum((unsigned char)p[-1]) && p[-1] != '_');
        int after  = !isalnum((unsigned char)p[hlen]) && p[hlen] != '_';
        if (before && after) return 1;
        p++;
    }
    (void)nick_mentioned; /* suppress unused-warning if only checked in bridge */
    return 0;
}

static const char *extract_sender(const char *line) {
    static char sbuf[32];
    char stripped[MSG_LEN + 16];
    const char *p;
    int i;
    strip_pipe_for_mention(stripped, sizeof(stripped), line);
    p = stripped;
    while (*p && !isalnum((unsigned char)*p) && *p != '_') p++;
    if (!*p) return NULL;
    for (i = 0; *p && i < (int)sizeof(sbuf) - 1; ++p) {
        if (*p == '>' || *p == ']' || *p == ')' || *p == '/' || *p == ' ' || *p == '<')
            break;
        sbuf[i++] = *p;
    }
    sbuf[i] = 0;
    return i > 0 ? sbuf : NULL;
}

/* ============================================================================
   TAB AUTOCOMPLETE
   ============================================================================ */

static int tab_complete(void) {
    int word_start, i;
    char partial[32];
    int plen;
    const char *tok;
    char users_copy[512];
    char *name, *rest;

    if (!g_tab_users[0] || g_input_len <= 0) return 0;

    word_start = g_input_len;
    while (word_start > 0 && g_input[word_start - 1] != ' ')
        --word_start;

    plen = g_input_len - word_start;
    if (plen <= 0) return 0;
    if (plen >= (int)sizeof(partial)) return 0;

    for (i = 0; i < plen; ++i)
        partial[i] = g_input[word_start + i];
    partial[plen] = 0;

    safe_copy(users_copy, sizeof(users_copy), g_tab_users);
    name = users_copy;
    while (name && *name) {
        rest = strchr(name, ',');
        if (rest) *rest = 0;
        tok = name;
        while (*tok == ' ' || *tok == '@' || *tok == '+' || *tok == '%' ||
               *tok == '&' || *tok == '~') ++tok;
        {
            int tlen = (int)strlen(tok);
            while (tlen > 0 && tok[tlen-1] == ' ') tlen--;
            if (tlen >= plen && strnicmp(tok, partial, (size_t)plen) == 0) {
                int new_len = word_start + tlen;
                if (new_len < (int)sizeof(g_input) - 2) {
                    int k;
                    for (k = 0; k < tlen; ++k)
                        g_input[word_start + k] = tok[k];
                    g_input[new_len] = 0;
                    g_input_len = new_len;
                    if (word_start == 0 && new_len + 1 < (int)sizeof(g_input) - 2) {
                        g_input[new_len] = ':';
                        g_input[new_len + 1] = ' ';
                        g_input[new_len + 2] = 0;
                        g_input_len = new_len + 2;
                    }
                    g_tab_hl_start = word_start;
                    g_tab_hl_end   = word_start + tlen;
                    return 1;
                }
            }
        }
        name = rest ? rest + 1 : NULL;
    }
    return 0;
}

/* ============================================================================
   SETTINGS FIELD COMMIT
   ============================================================================ */

static void commit_settings_edit(void) {
    char sanitized[128];

    switch (g_edit) {
    case EDIT_HANDLE:
        sanitize_nick(sanitized, sizeof(sanitized), g_input);
        if (!sanitized[0]) break;
        safe_copy(g_settings.nick, sizeof(g_settings.nick), sanitized);
        break;

    case EDIT_HCOLOR: {
        int c = atoi(g_input);
        if (c < 1 || c > 15) c = 11;
        g_settings.handle_color = c;
        break;
    }

    case EDIT_PREFIX:
        sanitize_field(g_settings.handle_prefix, sizeof(g_settings.handle_prefix),
                       g_input, 30);
        break;

    case EDIT_SUFFIX:
        sanitize_field(g_settings.handle_suffix, sizeof(g_settings.handle_suffix),
                       g_input, 30);
        break;

    case EDIT_TEXTCOLOR: {
        char cmd[32];
        int c = atoi(g_input);
        if (c < 1 || c > 15) c = 7;
        g_settings.text_color = c;
        snprintf(cmd, sizeof(cmd), "SET TEXTCOLOR %d", c);
        bridge_send_line(cmd);
        break;
    }

    case EDIT_QUITMSG:
        sanitize_field(g_settings.quitmsg, sizeof(g_settings.quitmsg),
                       g_input, 60);
        break;

    case EDIT_PARTMSG:
        sanitize_field(g_settings.partmsg, sizeof(g_settings.partmsg),
                       g_input, 60);
        break;

    default: break;
    }

    save_settings(g_bbs_user, &g_settings);
    g_edit = EDIT_NONE;
    g_input[0] = 0;
    g_input_len = 0;
}

/* ============================================================================
   MAIN LOOP
   ============================================================================ */

int main(int argc, char **argv) {
    dropfile_info_t drop;
    char status[64];
    char line[512];
    int ch;
    int stats_wait = 0;
    int i;
    int idle_count = 0;

    memset(&drop,  0, sizeof(drop));
    memset(g_input, 0, sizeof(g_input));
    safe_copy(status, sizeof(status), "READY");

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--local") == 0) {
            g_local_mode = 1;
            break;
        }
    }

    if (g_local_mode) {
        safe_copy(drop.user_name, sizeof(drop.user_name), "SysOp");
        safe_copy(drop.alias,     sizeof(drop.alias),     "SysOp");
        drop.comm_port = 0;
        fossil_set_local_mode();
    } else {
        const char *drop_path = dropfile_get_path_from_args(argc, argv);
        if (!dropfile_load(&drop, drop_path)) return 1;
    }

    safe_copy(g_bbs_user, sizeof(g_bbs_user),
              drop.alias[0] ? drop.alias : drop.user_name);

    fossil_set_port((unsigned short)drop.comm_port);
    if (!fossil_init()) return 1;

    if (drop.comm_port >= 0)
        bridge_set_node(drop.comm_port + 1);

    if (!bridge_connect_local()) {
        fossil_puts("Bridge connect failed.\r\n");
        fossil_deinit();
        return 1;
    }

    if (load_settings(g_bbs_user, &g_settings) && g_settings.nick[0]) {
        g_mode = MODE_MENU;
    } else {
        g_mode = MODE_HANDLE;
    }

    if (g_settings.handle_color < 0 || g_settings.handle_color > 15)
        g_settings.handle_color = 11;
    if (g_settings.text_color < 0 || g_settings.text_color > 15)
        g_settings.text_color = 7;
    if (g_settings.theme < 1 || g_settings.theme > 6)
        g_settings.theme = 1;

    if (g_mode == MODE_HANDLE)
        draw_handle_entry(&drop);
    else
        draw_main_menu(&drop);

    for (;;) {
        int got_bridge = 0;

        while (bridge_poll_line(line, sizeof(line))) {
            got_bridge = 1;

            if (strncmp(line, "STATUS ", 7) == 0) {
                safe_copy(status, sizeof(status), line + 7);
                safe_copy(g_chat_status, sizeof(g_chat_status), line + 7);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "CHAN ", 5) == 0) {
                safe_copy(g_chat_chan, sizeof(g_chat_chan), line + 5);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "TOPIC ", 6) == 0) {
                safe_copy(g_chat_topic, sizeof(g_chat_topic), line + 6);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "USERS ", 6) == 0) {
                safe_copy(g_chat_users, sizeof(g_chat_users), line + 6);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "LATENCY ", 8) == 0) {
                safe_copy(g_chat_latency, sizeof(g_chat_latency), line + 8);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "NICKECHO ", 9) == 0) {
                /* Bridge confirmed our nick change — update settings */
                sanitize_nick(g_settings.nick, sizeof(g_settings.nick), line + 9);
                save_settings(g_bbs_user, &g_settings);
                if (g_mode == MODE_CHAT) redraw_chat_input();
                continue;
            }

            if (strncmp(line, "CHATLIST ", 9) == 0) {
                safe_copy(g_tab_users, sizeof(g_tab_users), line + 9);
                continue;
            }

            if (strncmp(line, "STATS_RESULT ", 13) == 0) {
                stats_wait = 0;
                draw_stats(line + 13);
                g_mode = MODE_STATS;
                continue;
            }

            if (g_mode == MODE_CHAT) {
                if (strcmp(status, "DISCONNECTED") == 0 ||
                    strcmp(status, "OFFLINE")      == 0 ||
                    strcmp(status, "SOCKETERROR")  == 0) {
                    g_sent_quit = 1;
                    add_msg_wrapped("|12DISCONNECTED|07 Press ESC to return to menu.", CHAT_W);
                }
                if (line[0]) {
                    /* Track incoming PM sender for /r reply. */
                    if (strncmp(line, "|08(@|11", 9) == 0) {
                        const char *p = line + 9;
                        const char *end = p;
                        while (*end && *end != '|') end++;
                        if (end > p) {
                            int n = (int)(end - p);
                            char nm[32];
                            if (n >= (int)sizeof(nm)) n = sizeof(nm) - 1;
                            memcpy(nm, p, (size_t)n);
                            nm[n] = 0;
                            if (nm[0] && stricmp(nm, g_settings.nick) != 0)
                                safe_copy(g_last_dm_target, sizeof(g_last_dm_target), nm);
                        }
                    }

                    /* LISTING lines: /list, /names, /who (no timestamp). */
                    if (strncmp(line, "LISTING ", 8) == 0) {
                        add_msg_wrapped(line + 8, CHAT_W);
                        if (g_scroll_off > 0) g_new_while_scrolled++;
                        draw_chat_msgs();
                        continue;
                    }

                    /* MENTION bridge messages ignored in IRC build. */
                    if (strncmp(line, "MENTION ", 8) == 0) continue;

                    /* Twit filter: suppress messages from ignored nicks. */
                    if (g_settings.twit_count > 0 && is_mentionable_chat_line(line)) {
                        const char *twit_who = extract_sender(line);
                        if (twit_who && twit_has(twit_who)) {
                            continue;
                        }
                    }

                    {
                        char ts[8], tline[MSG_LEN + 16];
                        local_time_str(ts, sizeof(ts));
                        snprintf(tline, sizeof(tline), "|08[%s]|07 %s", ts, line);
                        add_msg_wrapped(tline, CHAT_W);
                    }
                }

                if (g_scroll_off > 0)
                    g_new_while_scrolled++;

                draw_chat_msgs();
            }
        }

        if (stats_wait > 0) {
            --stats_wait;
            if (stats_wait == 0) {
                draw_stats("0 0 0 0 (no response)");
                add_msg("|12TIMEOUT|07 No response from stats query.");
                g_mode = MODE_STATS;
            }
        }

        ch = fossil_getch_nonblock();
        if (ch < 0) {
            if (!got_bridge) {
                _asm { int 28h }
                idle_count++;
                delay(idle_count > 10 ? 50 : 20);
            } else {
                idle_count = 0;
            }
            continue;
        }
        idle_count = 0;

        switch (g_mode) {

        case MODE_HANDLE:
            g_input_max = 20;
            if (ch == '\r' || ch == '\n') {
                char sanitized[32];
                sanitize_nick(sanitized, sizeof(sanitized), g_input);
                if (sanitized[0]) {
                    safe_copy(g_settings.nick, sizeof(g_settings.nick), sanitized);
                    save_settings(g_bbs_user, &g_settings);
                    g_input[0] = 0; g_input_len = 0;
                    g_mode = MODE_MENU;
                    draw_main_menu(&drop);
                }
            } else if ((ch == 8 || ch == 127) && g_input_len > 0) {
                g_input[--g_input_len] = 0;
                draw_handle_entry(&drop);
            } else if (isprint((unsigned char)ch) && g_input_len < g_input_max) {
                g_input[g_input_len++] = (char)ch;
                g_input[g_input_len] = 0;
                draw_handle_entry(&drop);
            } else if (ch == 27) {
                goto exit_loop;
            }
            break;

        case MODE_MENU:
            if (ch == '1') {
                g_msg_count = 0;
                g_chat_chan[0] = 0; g_chat_topic[0] = 0;
                g_chat_users[0] = 0;
                safe_copy(g_chat_status, sizeof(g_chat_status), "CONNECTING");
                add_msg_wrapped("|14Connecting to IRC...|07", CHAT_W);
                add_msg_wrapped("|07Type |15/help|07 for commands, |15/join #channel|07 to enter a channel.", CHAT_W);
                g_sent_quit = 0;
                safe_copy(status, sizeof(status), "CONNECTING");
                send_connect();
                g_mode = MODE_CHAT;
                draw_chat();
            } else if (ch == '2') {
                g_net_idx = -1;
                g_net_sub = NS_PICK;
                g_net_custom_host[0] = 0;
                g_net_custom_port = 6667;
                g_input[0] = 0; g_input_len = 0;
                g_mode = MODE_NETWORKS;
                draw_networks_menu();
            } else if (ch == '3') {
                g_edit = EDIT_NONE;
                g_input[0] = 0; g_input_len = 0;
                g_mode = MODE_SETTINGS;
                draw_settings_menu();
            } else if (ch == '4') {
                draw_stats_waiting();
                bridge_send_line("QUICKSTATS");
                stats_wait = 200;
                g_mode = MODE_STATS;
            } else if (ch == '5') {
                g_help_page = 0;
                g_mode = MODE_HELP;
                draw_help();
            } else if (ch == '6' || ch == 'q' || ch == 'Q' || ch == 27) {
                goto exit_loop;
            }
            break;

        case MODE_NETWORKS:
            if (g_net_sub == NS_PICK) {
                if (ch == 27 || ch == '0') {
                    g_mode = MODE_MENU;
                    draw_main_menu(&drop);
                    break;
                }
                if (ch >= 'a' && ch < 'a' + NUM_NETWORKS) {
                    g_net_idx = ch - 'a';
                    if (g_networks[g_net_idx].port_plain > 0 &&
                        g_networks[g_net_idx].port_tls > 0) {
                        g_net_sub = NS_TLS_PROMPT;
                        draw_tls_prompt();
                    } else {
                        /* Single-port network: connect immediately. */
                        int use_tls = (g_networks[g_net_idx].port_tls > 0);
                        int port = use_tls ? g_networks[g_net_idx].port_tls
                                           : g_networks[g_net_idx].port_plain;
                        g_msg_count = 0;
                        g_chat_chan[0] = 0; g_chat_topic[0] = 0;
                        g_chat_users[0] = 0;
                        safe_copy(g_chat_status, sizeof(g_chat_status), "CONNECTING");
                        {
                            char m[128];
                            snprintf(m, sizeof(m),
                                     "|14Connecting to %s (%s:%d %s)...|07",
                                     g_networks[g_net_idx].label,
                                     g_networks[g_net_idx].host, port,
                                     use_tls ? "TLS" : "plain");
                            add_msg_wrapped(m, CHAT_W);
                        }
                        g_sent_quit = 0;
                        safe_copy(status, sizeof(status), "CONNECTING");
                        send_connect_explicit(g_networks[g_net_idx].host, port, use_tls);
                        g_mode = MODE_CHAT;
                        draw_chat();
                    }
                } else if (ch == 'a' + NUM_NETWORKS) {
                    /* Manual entry */
                    g_input[0] = 0; g_input_len = 0;
                    g_net_sub = NS_CUSTOM_HOST;
                    draw_custom_host();
                }
            } else if (g_net_sub == NS_TLS_PROMPT) {
                if (ch == 27) {
                    g_net_sub = NS_PICK;
                    draw_networks_menu();
                    break;
                }
                {
                    int use_tls = 1;  /* default */
                    int port;
                    if (ch == 'n' || ch == 'N') use_tls = 0;
                    else if (ch == 'y' || ch == 'Y' || ch == '\r' || ch == '\n') use_tls = 1;
                    else break;   /* ignore other keys */
                    port = use_tls ? g_networks[g_net_idx].port_tls
                                   : g_networks[g_net_idx].port_plain;
                    g_msg_count = 0;
                    g_chat_chan[0] = 0; g_chat_topic[0] = 0;
                    g_chat_users[0] = 0;
                    safe_copy(g_chat_status, sizeof(g_chat_status), "CONNECTING");
                    {
                        char m[128];
                        snprintf(m, sizeof(m),
                                 "|14Connecting to %s (%s:%d %s)...|07",
                                 g_networks[g_net_idx].label,
                                 g_networks[g_net_idx].host, port,
                                 use_tls ? "TLS" : "plain");
                        add_msg_wrapped(m, CHAT_W);
                    }
                    g_sent_quit = 0;
                    safe_copy(status, sizeof(status), "CONNECTING");
                    send_connect_explicit(g_networks[g_net_idx].host, port, use_tls);
                    g_mode = MODE_CHAT;
                    draw_chat();
                }
            } else if (g_net_sub == NS_CUSTOM_HOST) {
                if (ch == 27) {
                    g_net_sub = NS_PICK;
                    draw_networks_menu();
                } else if (ch == '\r' || ch == '\n') {
                    if (g_input_len > 0) {
                        safe_copy(g_net_custom_host, sizeof(g_net_custom_host), g_input);
                        g_input[0] = 0; g_input_len = 0;
                        g_net_sub = NS_CUSTOM_PORT;
                        draw_custom_port();
                    }
                } else if ((ch == 8 || ch == 127) && g_input_len > 0) {
                    g_input[--g_input_len] = 0;
                    draw_custom_host();
                } else if (isprint((unsigned char)ch) && g_input_len < 120) {
                    g_input[g_input_len++] = (char)ch;
                    g_input[g_input_len] = 0;
                    draw_custom_host();
                }
            } else if (g_net_sub == NS_CUSTOM_PORT) {
                if (ch == 27) {
                    g_net_sub = NS_CUSTOM_HOST;
                    safe_copy(g_input, sizeof(g_input), g_net_custom_host);
                    g_input_len = (int)strlen(g_input);
                    draw_custom_host();
                } else if (ch == '\r' || ch == '\n') {
                    int p = g_input_len > 0 ? atoi(g_input) : 6667;
                    if (p <= 0) p = 6667;
                    g_net_custom_port = p;
                    g_input[0] = 0; g_input_len = 0;
                    g_net_sub = NS_CUSTOM_TLS;
                    draw_custom_tls();
                } else if ((ch == 8 || ch == 127) && g_input_len > 0) {
                    g_input[--g_input_len] = 0;
                    draw_custom_port();
                } else if (ch >= '0' && ch <= '9' && g_input_len < 6) {
                    g_input[g_input_len++] = (char)ch;
                    g_input[g_input_len] = 0;
                    draw_custom_port();
                }
            } else if (g_net_sub == NS_CUSTOM_TLS) {
                if (ch == 27) {
                    g_net_sub = NS_PICK;
                    draw_networks_menu();
                    break;
                }
                {
                    int use_tls = 0;
                    if (ch == 'y' || ch == 'Y') use_tls = 1;
                    else if (ch == 'n' || ch == 'N' || ch == '\r' || ch == '\n') use_tls = 0;
                    else break;
                    g_msg_count = 0;
                    g_chat_chan[0] = 0; g_chat_topic[0] = 0;
                    g_chat_users[0] = 0;
                    safe_copy(g_chat_status, sizeof(g_chat_status), "CONNECTING");
                    {
                        char m[160];
                        snprintf(m, sizeof(m),
                                 "|14Connecting to %s:%d %s...|07",
                                 g_net_custom_host, g_net_custom_port,
                                 use_tls ? "(TLS)" : "(plain)");
                        add_msg_wrapped(m, CHAT_W);
                    }
                    g_sent_quit = 0;
                    safe_copy(status, sizeof(status), "CONNECTING");
                    send_connect_explicit(g_net_custom_host, g_net_custom_port, use_tls);
                    g_mode = MODE_CHAT;
                    draw_chat();
                }
            }
            break;

        case MODE_SETTINGS:
            if (g_edit == EDIT_NONE) {
                g_input[0] = 0; g_input_len = 0;
                if (ch == '1') { g_edit = EDIT_HANDLE;    g_input_max = 20; }
                else if (ch == '2') { g_edit = EDIT_HCOLOR;    g_input_max = 3;  }
                else if (ch == '3') { g_edit = EDIT_PREFIX;    g_input_max = 30; }
                else if (ch == '4') { g_edit = EDIT_SUFFIX;    g_input_max = 30; }
                else if (ch == '5') { g_edit = EDIT_TEXTCOLOR; g_input_max = 3;  }
                else if (ch == '6') { g_edit = EDIT_QUITMSG;   g_input_max = 60; }
                else if (ch == '7') { g_edit = EDIT_PARTMSG;   g_input_max = 60; }
                else if (ch == 'b' || ch == 'B' || ch == '0' || ch == 27) {
                    g_mode = MODE_MENU;
                    draw_main_menu(&drop);
                    break;
                }
                if (g_edit != EDIT_NONE)
                    draw_settings_menu();
            } else {
                if (ch == '\r' || ch == '\n') {
                    commit_settings_edit();
                    draw_settings_menu();
                } else if (ch == 27) {
                    g_edit = EDIT_NONE;
                    g_input[0] = 0; g_input_len = 0;
                    draw_settings_menu();
                } else if ((ch == 8 || ch == 127) && g_input_len > 0) {
                    g_input[--g_input_len] = 0;
                    draw_settings_menu();
                } else if (isprint((unsigned char)ch) && g_input_len < g_input_max) {
                    g_input[g_input_len++] = (char)ch;
                    g_input[g_input_len] = 0;
                    draw_settings_menu();
                }
            }
            break;

        case MODE_STATS:
            stats_wait = 0;
            g_mode = MODE_MENU;
            draw_main_menu(&drop);
            break;

        case MODE_HELP:
            if (g_help_page == 0) {
                if (ch >= '1' && ch <= '4') {
                    g_help_page = ch - '0';
                    draw_help();
                } else if (ch == 'b' || ch == 'B' || ch == 27 || ch == '0') {
                    g_mode = MODE_MENU;
                    draw_main_menu(&drop);
                }
            } else {
                g_help_page = 0;
                draw_help();
            }
            break;

        case MODE_CHAT:
            {
                {
                    char _pr[64]; int _pv;
                    snprintf(_pr, sizeof(_pr), "%s|%02d%s%s|07 ",
                             g_settings.handle_prefix, g_settings.handle_color,
                             g_settings.nick, g_settings.handle_suffix);
                    _pv = pipe_vis_len(_pr);
                    g_chat_view_w = SCREEN_W - _pv - 1;
                    if (g_chat_view_w < 10) g_chat_view_w = 10;
                }
                g_input_max = 400;
                if (g_input_max >= (int)sizeof(g_input) - 1)
                    g_input_max = (int)sizeof(g_input) - 2;
            }

            if (ch == 0) {
                int scan = fossil_getch_nonblock();
                if (scan < 0) break;
                if (scan == 73) {
                    int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                    g_scroll_off += CHAT_MSG_ROWS;
                    if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
                    draw_chat_msgs();
                } else if (scan == 81) {
                    g_scroll_off -= CHAT_MSG_ROWS;
                    if (g_scroll_off < 0) g_scroll_off = 0;
                    if (g_scroll_off == 0) g_new_while_scrolled = 0;
                    draw_chat_msgs();
                } else if (scan == 72) {
                    int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                    if (max_scroll > 0) {
                        g_scroll_off++;
                        if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
                        draw_chat_msgs();
                    }
                } else if (scan == 80) {
                    if (g_scroll_off > 0) {
                        g_scroll_off--;
                        if (g_scroll_off == 0) g_new_while_scrolled = 0;
                        draw_chat_msgs();
                    }
                } else if (scan == 71) {
                    int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                    if (max_scroll > 0) {
                        g_scroll_off = max_scroll;
                        draw_chat_msgs();
                    }
                } else if (scan == 79) {
                    if (g_scroll_off > 0) {
                        g_scroll_off = 0;
                        g_new_while_scrolled = 0;
                        draw_chat_msgs();
                    }
                }
                break;
            }

            if (ch == 27) {
                int ch2, retries;
                ch2 = -1;
                for (retries = 0; retries < 25; ++retries) {
                    ch2 = fossil_getch_nonblock();
                    if (ch2 >= 0) break;
                    delay(2);
                }
                if (ch2 == '[' || ch2 == 'O') {
                    int ch3 = -1, ch4 = -1;
                    for (retries = 0; retries < 25; ++retries) {
                        ch3 = fossil_getch_nonblock();
                        if (ch3 >= 0) break;
                        delay(2);
                    }
                    if (ch3 == '5' || ch3 == 'I') {
                        int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                        for (retries = 0; retries < 25; ++retries) {
                            ch4 = fossil_getch_nonblock(); (void)ch4;
                            if (ch4 >= 0) break;
                            delay(2);
                        }
                        g_scroll_off += CHAT_MSG_ROWS;
                        if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
                        draw_chat_msgs();
                    } else if (ch3 == '6' || ch3 == 'G') {
                        for (retries = 0; retries < 25; ++retries) {
                            ch4 = fossil_getch_nonblock(); (void)ch4;
                            if (ch4 >= 0) break;
                            delay(2);
                        }
                        g_scroll_off -= CHAT_MSG_ROWS;
                        if (g_scroll_off < 0) g_scroll_off = 0;
                        if (g_scroll_off == 0) g_new_while_scrolled = 0;
                        draw_chat_msgs();
                    } else if (ch3 == 'A') {
                        if (g_scroll_off > 0 ||
                            (g_msg_count > CHAT_MSG_ROWS && g_input_len == 0)) {
                            int max_scroll = g_msg_count > CHAT_MSG_ROWS ?
                                             g_msg_count - CHAT_MSG_ROWS : 0;
                            g_scroll_off++;
                            if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
                            draw_chat_msgs();
                        } else if (g_sent_hist_count > 0) {
                            if (g_hist_browse < 0) {
                                safe_copy(g_hist_saved, sizeof(g_hist_saved), g_input);
                                g_hist_browse = 0;
                            } else if (g_hist_browse < g_sent_hist_count - 1) {
                                g_hist_browse++;
                            }
                            safe_copy(g_input, sizeof(g_input), g_sent_hist[g_hist_browse]);
                            g_input_len = (int)strlen(g_input);
                            g_tab_hl_start = -1; g_tab_hl_end = -1;
                            redraw_chat_input();
                        }
                    } else if (ch3 == 'B') {
                        if (g_scroll_off > 0) {
                            g_scroll_off--;
                            if (g_scroll_off == 0) g_new_while_scrolled = 0;
                            draw_chat_msgs();
                        } else if (g_hist_browse >= 0) {
                            g_hist_browse--;
                            if (g_hist_browse < 0) {
                                safe_copy(g_input, sizeof(g_input), g_hist_saved);
                            } else {
                                safe_copy(g_input, sizeof(g_input), g_sent_hist[g_hist_browse]);
                            }
                            g_input_len = (int)strlen(g_input);
                            g_tab_hl_start = -1; g_tab_hl_end = -1;
                            redraw_chat_input();
                        }
                    } else if (ch3 == 'C') {
                        char cmd[32];
                        g_settings.text_color = (g_settings.text_color + 1) % 16;
                        save_settings(g_bbs_user, &g_settings);
                        snprintf(cmd, sizeof(cmd),"SET TEXTCOLOR %d", g_settings.text_color);
                        bridge_send_line(cmd);
                        redraw_chat_input();
                    } else if (ch3 == 'D') {
                        char cmd[32];
                        g_settings.text_color = (g_settings.text_color + 15) % 16;
                        save_settings(g_bbs_user, &g_settings);
                        snprintf(cmd, sizeof(cmd),"SET TEXTCOLOR %d", g_settings.text_color);
                        bridge_send_line(cmd);
                        redraw_chat_input();
                    } else if (ch3 == '1' || ch3 == 'H') {
                        for (retries = 0; retries < 15; ++retries) {
                            ch4 = fossil_getch_nonblock(); (void)ch4;
                            if (ch4 >= 0) break;
                            delay(2);
                        }
                        {
                            int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                            if (max_scroll > 0) {
                                g_scroll_off = max_scroll;
                                draw_chat_msgs();
                            }
                        }
                    } else if (ch3 == '4' || ch3 == 'F' || ch3 == 'K') {
                        for (retries = 0; retries < 15; ++retries) {
                            ch4 = fossil_getch_nonblock(); (void)ch4;
                            if (ch4 >= 0) break;
                            delay(2);
                        }
                        if (g_scroll_off > 0) {
                            g_scroll_off = 0;
                            g_new_while_scrolled = 0;
                            draw_chat_msgs();
                            redraw_chat_input();
                        }
                    }
                    ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col());
                    break;
                }
                if (ch2 >= 0) {
                    ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col());
                    break;
                }
                if (g_scroll_off > 0) {
                    g_scroll_off = 0;
                    g_new_while_scrolled = 0;
                    draw_chat_msgs();
                    redraw_chat_input();
                    break;
                }
                g_scroll_off = 0;
                g_new_while_scrolled = 0;
                g_hist_browse = -1;
                send_quit_once();
                g_mode = MODE_MENU;
                draw_main_menu(&drop);
                break;
            }

            if (ch == '\t') {
                if (tab_complete())
                    redraw_chat_input();
                break;
            }

            if (ch == '\r' || ch == '\n') {
                g_tab_hl_start = -1; g_tab_hl_end = -1;
                g_hist_browse = -1;
                g_input[g_input_len] = 0;
                if (g_input_len > 0) {
                    int is_secret =
                        strncmp(g_input, "/identify ", 10) == 0 ||
                        strncmp(g_input, "/register ", 10) == 0 ||
                        strncmp(g_input, "/oper ", 6) == 0;
                    if (!is_secret) {
                        int hi;
                        for (hi = SENT_HIST_MAX - 1; hi > 0; --hi)
                            safe_copy(g_sent_hist[hi], sizeof(g_sent_hist[hi]), g_sent_hist[hi-1]);
                        safe_copy(g_sent_hist[0], sizeof(g_sent_hist[0]), g_input);
                        if (g_sent_hist_count < SENT_HIST_MAX) g_sent_hist_count++;
                    }

                    if (g_input[0] == '/') {
                        handle_slash_command(g_input);
                        if (g_return_to_menu) {
                            g_return_to_menu = 0;
                            g_scroll_off = 0;
                            g_new_while_scrolled = 0;
                            g_hist_browse = -1;
                            send_quit_once();
                            g_mode = MODE_MENU;
                            g_input_len = 0;
                            g_input[0] = 0;
                            draw_main_menu(&drop);
                            break;
                        }
                        draw_chat_msgs();
                    } else {
                        char cmd[512];
                        g_scroll_off = 0;
                        g_new_while_scrolled = 0;
                        snprintf(cmd, sizeof(cmd),"SEND %s", g_input);
                        bridge_send_line(cmd);
                    }
                    g_input_len = 0;
                    g_input[0] = 0;
                    draw_char_count_only();
                }
                redraw_chat_input();
            } else if ((ch == 8 || ch == 127) && g_input_len > 0) {
                g_tab_hl_start = -1; g_tab_hl_end = -1;
                g_input[--g_input_len] = 0;
                ansi_color_code(g_settings.text_color);
                if (g_input_len + 1 > g_chat_view_w) {
                    redraw_chat_input();
                } else {
                    fossil_puts("\b \b");
                }
                draw_char_count_only();
                ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col());
            } else if (isprint((unsigned char)ch) &&
                       g_input_len < g_input_max) {
                int ms, me, vis;
                g_tab_hl_start = -1; g_tab_hl_end = -1;
                g_input[g_input_len++] = (char)ch;
                g_input[g_input_len]   = 0;
                masked_range(&ms, &me);
                vis = ((g_input_len - 1) >= ms && (g_input_len - 1) < me)
                      ? '*' : (unsigned char)ch;
                if (g_input_len > g_chat_view_w) {
                    redraw_chat_input();
                } else {
                    ansi_color_code(g_settings.text_color);
                    fossil_putch((unsigned char)vis);
                }
                draw_char_count_only();
                ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col());
            }
            break;
        }
    }

exit_loop:
    send_quit_once();
    bridge_close();
    fossil_deinit();
    return 0;
}
