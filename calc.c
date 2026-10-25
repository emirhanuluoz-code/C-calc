// Emirhan Uluoz 251ADAB050
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: lecture presentations

/*
 * Overall design: the input file is read into memory once, and a small
 * parser evaluates each line directly from that buffer while parsing.
 * Nothing is copied line by line and no token list is built, so a large
 * file costs one read and one pass.
 */

#define _POSIX_C_SOURCE 200809L // mkdir() is POSIX, not ISO C; this makes
                                // <sys/stat.h> declare it under -std=c17

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STUDENT_NAME     "emirhan"
#define STUDENT_LASTNAME "uluoz"
#define STUDENT_ID       "251ADB050"

/* =================== Tokenizer & Position Tracking ===================
 *
 * - Global index: the parser works on the whole file buffer, so `pos` is
 *   an absolute index, not a per-line one. The reported position is
 *   simply `pos + 1` (positions are 1-based).
 * - Newlines are counted automatically as `pos` moves over them, so no
 *   extra line/column bookkeeping is needed.
 * - skip_spaces() runs before every token is examined. When parsing
 *   fails, `pos` therefore already points at the first character of the
 *   token that could not be used, which is exactly the required position.
 */

static const char *buf;   // whole input file
static size_t len;        // its length in bytes
static size_t pos;        // index of the next unread character
static size_t err_pos;    // 1-based position of the first error; 0 = none

/* Only the first error counts, so a later call must not
   overwrite a position that is already stored. */
static void fail_at(size_t index) {
    if (err_pos == 0) err_pos = index + 1;
}

/* only spaces and tabs separate tokens. A '\n' is not skipped
   here because it ends the expression and is itself a reportable
   position ("missing operand at end of line"). */
static void skip_spaces(void) {
    while (pos < len && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* ============================ Parser ============================
 *
 * Grammar:
 *   expr := NUMBER { ('+' | '-') NUMBER }
 *
 * - + and - have the same precedence and are left-associative
 *   (7 - 2 - 1 is (7 - 2) - 1), so a simple left-to-right loop that
 *   applies each operator as soon as its right operand is read gives
 *   the correct result.
 * - After an error, err_pos is set and the loop stops; the caller then
 *   prints only "ERROR:<pos>".
 */

/* Reads one number. If the current token is not a digit, an operand was
   expected but something else was found. Because pos points exactly at
   that token
   */
static long long parse_number(void) {
    skip_spaces();
    if (pos >= len || !is_digit(buf[pos])) {
        fail_at(pos);
        return 0;
    }
    long long v = 0;
    while (pos < len && is_digit(buf[pos])) {
        v = v * 10 + (buf[pos] - '0');
        pos++;
    }
    return v;
}

static long long parse_expr(void) {
    long long v = parse_number();
    while (!err_pos) {
        skip_spaces();
        if (pos >= len || (buf[pos] != '+' && buf[pos] != '-')) break;
        char op = buf[pos++];
        long long r = parse_number();
        if (err_pos) break;
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

/* parse_expr stops at the first character it does not understand. Here
   we check that the whole line was used: anything left before the '\n'
   (for example "2 3" or "2 & 3") is an error at that character. */
static long long parse_line(void) {
    long long v = parse_expr();
    if (!err_pos) {
        skip_spaces();
        if (pos < len && buf[pos] != '\n') fail_at(pos);
    }
    return v;
}

/* ========================= Output Buffer =========================
 *
 * Results are collected in memory because an error on a later line must
 * replace ALL earlier results. The buffer doubles when full, so
 * the total copying cost stays linear even for large files.
 */
static char  *out;
static size_t out_len, out_cap;

static void out_append(const char *s) {
    size_t n = strlen(s);
    if (out_len + n + 1 > out_cap) {
        size_t new_cap = (out_len + n + 1) * 2;
        char *p = realloc(out, new_cap);
        if (!p) { perror("calc"); free(out); exit(1); }
        out = p;
        out_cap = new_cap;
    }
    memcpy(out + out_len, s, n + 1);
    out_len += n;
}

/* Evaluates the whole buffer; afterwards `out` holds the file's content. */
static void evaluate(void) {
    char text[64];
    pos = 0;
    err_pos = 0;
    out_len = 0;
    out_append(""); // makes out a valid empty string even for empty input

    while (pos < len && !err_pos) {
        size_t line_start = pos;
        skip_spaces();
        if (pos >= len) break;                       // trailing blanks
        if (buf[pos] == '\n') { pos++; continue; }   // blank line: no output
        pos = line_start; // let the parser do its own space skipping
        long long v = parse_line();
        if (err_pos) break;
        snprintf(text, sizeof text, "%lld\n", v);
        out_append(text);
        if (pos < len) pos++; // parse_line stopped at this line's '\n'
    }

    if (err_pos) {
        out_len = 0; // discard partial results
        snprintf(text, sizeof text, "ERROR:%zu\n", err_pos);
        out_append(text);
    }
}

/* ========================= File handling ========================= */

/* Reads a whole file in one pass. Like the output buffer, the read buffer
   doubles when full, so large files are not copied over and over. */
static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, used = 0, got;
    char *data = malloc(cap);
    while (data && (got = fread(data + used, 1, cap - used, f)) > 0) {
        used += got;
        if (used == cap) {
            char *p = realloc(data, cap * 2);
            if (!p) { free(data); data = NULL; break; }
            data = p;
            cap *= 2;
        }
    }
    fclose(f);
    *n = used;
    return data;
}

/* "../in/task1.txt" -> "task1": the output name is built from the bare
   input name without folders or the .txt extension. */
static void base_name(const char *path, char *dst, size_t size) {
    const char *slash = strrchr(path, '/');
    snprintf(dst, size, "%s", slash ? slash + 1 : path);
    char *dot = strrchr(dst, '.');
    if (dot && strcmp(dot, ".txt") == 0) *dot = '\0';
}

/* An output folder that already exists is fine, so EEXIST is
   not treated as an error. */
static int make_dir(const char *dir) {
    if (mkdir(dir, 0755) == 0 || errno == EEXIST) return 0;
    fprintf(stderr, "calc: cannot create folder '%s': %s\n", dir, strerror(errno));
    return -1;
}

/* Processes one input file. Returns 0 on success, -1 when a file cannot
   be read or written. An ERROR:<pos> result is a normal success. */
static int process_file(const char *in_path, const char *out_dir) {
    size_t n;
    char *data = read_file(in_path, &n);
    if (!data) {
        fprintf(stderr, "calc: cannot read '%s': %s\n", in_path, strerror(errno));
        return -1;
    }
    buf = data;
    len = n;
    evaluate();
    free(data);
    buf = NULL;

    char base[256], out_path[1024];
    base_name(in_path, base, sizeof base);
    snprintf(out_path, sizeof out_path, "%s/%s_%s_%s_%s.txt",
             out_dir, base, STUDENT_NAME, STUDENT_LASTNAME, STUDENT_ID);

    if (make_dir(out_dir) != 0) return -1;
    FILE *f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "calc: cannot write '%s': %s\n", out_path, strerror(errno));
        return -1;
    }
    fputs(out, f);
    if (fclose(f) != 0) {
        fprintf(stderr, "calc: cannot write '%s': %s\n", out_path, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    // Only "calc -o OUTDIR input.txt" is supported at this grade.
    const char *out_dir = NULL, *input = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_dir = argv[++i];
        else input = argv[i];
    }
    if (!input || !out_dir) {
        fprintf(stderr, "usage: calc -o OUTDIR input.txt\n");
        return 1;
    }
    int rc = process_file(input, out_dir) == 0 ? 0 : 1;
    free(out);
    return rc;
}