#ifndef BTA_JSON_H
#define BTA_JSON_H
#include "bta_types.h"
/* Bounded JSON writer. Everything is appended into a caller-owned buffer that
 * is never overrun and always left NUL-terminated. Values are numbers and
 * fixed internal strings only, so the output is always valid JSON. */
struct writer { char *b; unsigned used, cap; };

static void append(struct writer *w, const char *s) {
    while (*s && w->used + 1 < w->cap) w->b[w->used++] = *s++;
    w->b[w->used] = 0;
}
static void number(struct writer *w, long n) {
    char b[24];
    unsigned i = 0;
    u64 v;
    if (n < 0) { append(w, "-"); v = 0 - (u64)n; } else v = n;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) { char s[2] = {b[--i], 0}; append(w, s); }
}
/* Append `,"key":value` — the leading comma suits objects opened with a first
 * field already written (as the status and timing objects are). */
static void field(struct writer *w, const char *key, long value) {
    append(w, ",\"");
    append(w, key);
    append(w, "\":");
    number(w, value);
}
static void quoted(struct writer *w, const char *s) {
    append(w, "\"");
    for (unsigned i = 0; s[i] && i < 127; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') append(w, "\\");
        char b[2] = {(c >= 32 && c < 127) ? c : '?', 0};
        append(w, b);
    }
    append(w, "\"");
}
#endif
