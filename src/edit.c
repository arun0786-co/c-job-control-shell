#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*
 * A small line editor. The terminal normally collects a line and lets you
 * only rub out the end of it. To do more the shell turns that off (raw mode),
 * so that it gets each key as it is pressed, and it has to draw everything,
 * including the characters typed. Raw mode is on only while a line is being
 * edited, so programs run with the terminal as they expect it.
 */

typedef struct {
    char *buf;
    size_t len, cap;
    size_t pos;          /* the cursor, as a byte offset into buf */
    const char *prompt;
    int hist;            /* history entry shown; history_count() means the new line */
    char *draft;         /* what was being typed before moving into the history */
} editor_t;

static void put_str(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void puts_raw(const char *s)
{
    put_str(s, strlen(s));
}

static int terminal_columns(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* UTF-8: bytes of the form 10xxxxxx continue a character */
static int continues(char c)
{
    return ((unsigned char)c & 0xC0) == 0x80;
}

static size_t columns_of(const char *s, size_t n)
{
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (!continues(s[i]))
            w++;
    return w;
}

/* Draw the prompt and the part of the line that fits, with the cursor in view.
   A line too long for the terminal scrolls sideways, which is far simpler than
   tracking how it wraps. */
static void show(const char *prompt, const char *buf, size_t len, size_t pos)
{
    size_t cols = (size_t)terminal_columns();
    size_t plen = columns_of(prompt, strlen(prompt));
    size_t start = 0;
    size_t cursor = columns_of(buf, pos);
    /* a prompt that takes most of the width leaves room for a few characters */
    size_t room = cols > plen + 8 ? cols - plen - 1 : 8;
    while (cursor >= room) {
        do {
            start++;
        } while (start < len && continues(buf[start]));
        cursor--;
    }
    size_t end = start, shown = 0;
    while (end < len && shown < room) {
        end++;
        while (end < len && continues(buf[end]))
            end++;
        shown++;
    }
    char tail[32];
    puts_raw("\r");
    puts_raw(prompt);
    put_str(buf + start, end - start);
    puts_raw("\x1b[0K\r");
    snprintf(tail, sizeof tail, "\x1b[%zuC", plen + cursor);
    if (plen + cursor > 0)
        puts_raw(tail);
}

static void refresh(const editor_t *e)
{
    show(e->prompt, e->buf, e->len, e->pos);
}

static void reserve(editor_t *e, size_t extra)
{
    if (e->len + extra + 1 > e->cap) {
        while (e->len + extra + 1 > e->cap)
            e->cap = e->cap ? e->cap * 2 : 128;
        e->buf = xrealloc(e->buf, e->cap);
    }
}

static void insert(editor_t *e, const char *s, size_t n)
{
    reserve(e, n);
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
    memcpy(e->buf + e->pos, s, n);
    e->len += n;
    e->pos += n;
    e->buf[e->len] = '\0';
}

static void delete_range(editor_t *e, size_t from, size_t to)
{
    memmove(e->buf + from, e->buf + to, e->len - to);
    e->len -= to - from;
    if (e->pos >= to)
        e->pos -= to - from;
    else if (e->pos > from)
        e->pos = from;
    e->buf[e->len] = '\0';
}

static size_t char_before(const editor_t *e, size_t i)
{
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && continues(e->buf[i]))
        i--;
    return i;
}

static size_t char_after(const editor_t *e, size_t i)
{
    if (i >= e->len)
        return e->len;
    i++;
    while (i < e->len && continues(e->buf[i]))
        i++;
    return i;
}

static size_t word_before(const editor_t *e, size_t i)
{
    while (i > 0 && isspace((unsigned char)e->buf[i - 1]))
        i--;
    while (i > 0 && !isspace((unsigned char)e->buf[i - 1]))
        i--;
    return i;
}

static size_t word_after(const editor_t *e, size_t i)
{
    while (i < e->len && isspace((unsigned char)e->buf[i]))
        i++;
    while (i < e->len && !isspace((unsigned char)e->buf[i]))
        i++;
    return i;
}

static void set_text(editor_t *e, const char *s)
{
    size_t n = strlen(s);
    e->len = e->pos = 0;
    reserve(e, n);
    memcpy(e->buf, s, n);
    e->len = e->pos = n;
    e->buf[n] = '\0';
}

/* walk the history; the line being typed is kept as a draft to come back to */
static void history_move(editor_t *e, int delta)
{
    int target = e->hist + delta;
    if (target < 0 || target > history_count())
        return;
    if (e->hist == history_count()) {
        free(e->draft);
        e->draft = xstrdup(e->buf);
    }
    e->hist = target;
    set_text(e, target == history_count() ? e->draft : history_get(target));
}

/* a key that may be followed by more within a moment: an arrow key arrives as
   ESC [ A all at once, where the Esc key arrives alone */
static int read_key_soon(char *c)
{
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&p, 1, 50) <= 0)
        return 0;
    return read(STDIN_FILENO, c, 1) == 1;
}

enum { KEY_NONE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_DELETE,
       KEY_WORD_LEFT, KEY_WORD_RIGHT };

static int read_escape(void)
{
    char a, b;
    if (!read_key_soon(&a))
        return KEY_NONE;
    if (a == 'b')
        return KEY_WORD_LEFT;
    if (a == 'f')
        return KEY_WORD_RIGHT;
    if (a != '[' && a != 'O')
        return KEY_NONE;
    if (!read_key_soon(&b))
        return KEY_NONE;
    if (b >= '0' && b <= '9') {
        /* ESC [ 3 ~ or ESC [ 1 ; 5 C: read to the final letter */
        int first = b - '0', mod = 0, in_mod = 0;
        char c;
        while (read_key_soon(&c)) {
            if (c == ';') {
                in_mod = 1;
            } else if (c >= '0' && c <= '9') {
                if (in_mod)
                    mod = mod * 10 + (c - '0');
                else
                    first = first * 10 + (c - '0');
            } else {
                b = c;
                break;
            }
        }
        if (b == '~') {
            if (first == 3)
                return KEY_DELETE;
            if (first == 1 || first == 7)
                return KEY_HOME;
            if (first == 4 || first == 8)
                return KEY_END;
            return KEY_NONE;
        }
        if (mod == 5 && b == 'C')
            return KEY_WORD_RIGHT;
        if (mod == 5 && b == 'D')
            return KEY_WORD_LEFT;
    }
    switch (b) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default: return KEY_NONE;
    }
}

static int find_history(const char *query, int before)
{
    for (int i = before - 1; i >= 0; i--)
        if (strstr(history_get(i), query) != NULL)
            return i;
    return -1;
}

/* Ctrl-R: type to narrow, Ctrl-R again for an older match, Enter to run, any
   movement key to keep editing it, Ctrl-G to give up. Returns 1 if the line
   should run at once. */
static int search_history(editor_t *e)
{
    char query[256] = "";
    size_t qlen = 0;
    int at = history_count(), found = -1;
    char *saved = xstrdup(e->buf);
    size_t saved_pos = e->pos;
    for (;;) {
        char prompt[300];
        const char *text = found >= 0 ? history_get(found) : "";
        snprintf(prompt, sizeof prompt, "(search)'%s': ", query);
        show(prompt, text, strlen(text), strlen(text));
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            c = 7;
        if (c == 18) { /* Ctrl-R */
            int next = find_history(query, found >= 0 ? found : at);
            if (next >= 0)
                found = next;
        } else if (c == 127 || c == 8) {
            if (qlen > 0)
                query[--qlen] = '\0';
            found = qlen > 0 ? find_history(query, at) : -1;
        } else if (c == 7 || c == 3) { /* Ctrl-G or Ctrl-C */
            set_text(e, saved);
            e->pos = saved_pos;
            free(saved);
            return 0;
        } else if (c == '\r' || c == '\n') {
            if (found >= 0)
                set_text(e, history_get(found));
            free(saved);
            return 1;
        } else if ((unsigned char)c >= 32 && c != 127) {
            if (qlen + 1 < sizeof query) {
                query[qlen++] = c;
                query[qlen] = '\0';
                found = find_history(query, at);
            }
        } else {
            /* Esc, an arrow, Ctrl-A...: keep the match and go on editing */
            if (c == 27)
                read_escape();
            if (found >= 0)
                set_text(e, history_get(found));
            free(saved);
            return 0;
        }
    }
}

static struct termios original;

static int enter_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &original) < 0)
        return -1;
    struct termios raw = original;
    /* keys arrive one by one and unechoed. ISIG off makes Ctrl-C a plain key
       here: there is no job to send it to, only a line to abandon. */
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* TCSANOW, not TCSADRAIN: draining waits until the terminal has read all our
   output, and a Ctrl-C pressed during that wait would arrive while ISIG is still
   off, as a plain character instead of a signal for the job about to start */
static void leave_raw(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
}

static void complete(editor_t *e, int *listed);

/* returns a line the caller must free, or NULL at end of input */
char *edit_line(const char *prompt)
{
    editor_t e;
    memset(&e, 0, sizeof e);
    e.prompt = prompt;
    e.hist = history_count();
    reserve(&e, 1);
    e.buf[0] = '\0';
    if (enter_raw() < 0) {
        free(e.buf);
        return NULL;
    }
    int finished = 0, eof = 0, listed = 0;
    refresh(&e);
    while (!finished) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            eof = 1;
            break;
        }
        switch (c) {
        case '\r':
        case '\n':
            finished = 1;
            break;
        case 1: /* Ctrl-A */
            e.pos = 0;
            break;
        case 5: /* Ctrl-E */
            e.pos = e.len;
            break;
        case 2: /* Ctrl-B */
            e.pos = char_before(&e, e.pos);
            break;
        case 6: /* Ctrl-F */
            e.pos = char_after(&e, e.pos);
            break;
        case 3: /* Ctrl-C: drop this line and start a new one */
            puts_raw("^C\r\n");
            e.len = e.pos = 0;
            e.buf[0] = '\0';
            e.hist = history_count();
            break;
        case 4: /* Ctrl-D: end of input on an empty line, else delete */
            if (e.len == 0) {
                eof = 1;
                finished = 1;
            } else if (e.pos < e.len) {
                delete_range(&e, e.pos, char_after(&e, e.pos));
            }
            break;
        case 8:
        case 127:
            if (e.pos > 0)
                delete_range(&e, char_before(&e, e.pos), e.pos);
            break;
        case 9:
            complete(&e, &listed);
            refresh(&e);
            continue; /* keeps listed set, for a second Tab */
        case 11: /* Ctrl-K */
            delete_range(&e, e.pos, e.len);
            break;
        case 21: /* Ctrl-U */
            delete_range(&e, 0, e.pos);
            break;
        case 23: /* Ctrl-W */
            delete_range(&e, word_before(&e, e.pos), e.pos);
            break;
        case 12: /* Ctrl-L */
            puts_raw("\x1b[H\x1b[2J");
            break;
        case 14: /* Ctrl-N */
            history_move(&e, 1);
            break;
        case 16: /* Ctrl-P */
            history_move(&e, -1);
            break;
        case 18: /* Ctrl-R */
            if (search_history(&e))
                finished = 1;
            break;
        case 27:
            switch (read_escape()) {
            case KEY_LEFT: e.pos = char_before(&e, e.pos); break;
            case KEY_RIGHT: e.pos = char_after(&e, e.pos); break;
            case KEY_UP: history_move(&e, -1); break;
            case KEY_DOWN: history_move(&e, 1); break;
            case KEY_HOME: e.pos = 0; break;
            case KEY_END: e.pos = e.len; break;
            case KEY_WORD_LEFT: e.pos = word_before(&e, e.pos); break;
            case KEY_WORD_RIGHT: e.pos = word_after(&e, e.pos); break;
            case KEY_DELETE:
                if (e.pos < e.len)
                    delete_range(&e, e.pos, char_after(&e, e.pos));
                break;
            default: break;
            }
            break;
        default:
            /* bytes from 0x80 up are parts of UTF-8 characters */
            if ((unsigned char)c >= 32)
                insert(&e, &c, 1);
            break;
        }
        listed = 0;
        refresh(&e);
    }
    e.pos = e.len; /* so a long line is left showing its end */
    refresh(&e);
    puts_raw("\r\n");
    leave_raw();
    free(e.draft);
    if (eof) {
        free(e.buf);
        return NULL;
    }
    return e.buf;
}

/* ---- Tab ---- */

/* a name goes back into the line with whatever the shell would act on escaped */
static void insert_escaped(editor_t *e, const char *s)
{
    /* ~ means something only at the start of a word, and ~/ is what was typed */
    if (s[0] == '~' && s[1] != '/' && s[1] != '\0')
        insert(e, "\\", 1);
    if (s[0] == '~') {
        insert(e, s, 1);
        s++;
    }
    for (; *s != '\0'; s++) {
        if (strchr(" \t'\"\\$&|;<>()*?[]#", *s) != NULL)
            insert(e, "\\", 1);
        insert(e, s, 1);
    }
}

static void complete(editor_t *e, int *listed)
{
    size_t start;
    strlist_t found = { NULL, 0 };
    complete_word(e->buf, e->pos, &start, &found);
    if (found.n == 0) {
        puts_raw("\a");
    } else if (found.n == 1) {
        const char *one = found.items[0];
        delete_range(e, start, e->pos);
        if (one[0] == '$') {
            insert(e, one, strlen(one));
        } else {
            insert_escaped(e, one);
            if (one[strlen(one) - 1] != '/')
                insert(e, " ", 1);
        }
    } else {
        /* as much as they all have in common */
        size_t common = strlen(found.items[0]);
        for (int i = 1; i < found.n; i++) {
            size_t k = 0;
            while (k < common && found.items[i][k] == found.items[0][k])
                k++;
            common = k;
        }
        char *prefix = xstrndup(found.items[0], common);
        char *typed = complete_unescape(e->buf + start, e->pos - start);
        if (common > strlen(typed)) {
            delete_range(e, start, e->pos);
            if (prefix[0] == '$')
                insert(e, prefix, common);
            else
                insert_escaped(e, prefix);
            *listed = 0;
        } else if (*listed || found.n <= 40) {
            /* nothing more can be filled in: show the choices */
            int cols = terminal_columns(), used = 0;
            puts_raw("\r\n");
            for (int i = 0; i < found.n; i++) {
                int w = (int)strlen(found.items[i]) + 2;
                if (used > 0 && used + w > cols) {
                    puts_raw("\r\n");
                    used = 0;
                }
                puts_raw(found.items[i]);
                puts_raw("  ");
                used += w;
            }
            puts_raw("\r\n");
            *listed = 0;
        } else {
            char msg[64];
            snprintf(msg, sizeof msg, "\r\n%d choices; press Tab again to see them\r\n", found.n);
            puts_raw(msg);
            *listed = 1;
        }
        free(typed);
        free(prefix);
    }
    strlist_free(&found);
}
