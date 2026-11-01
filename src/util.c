#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n);
    if (p == NULL && n > 0) {
        perror("realloc");
        exit(1);
    }
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xrealloc(NULL, n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

/* returns 1 if s is a whole base-10 integer, storing it in *out */
int parse_number(const char *s, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0)
        return 0;
    *out = v;
    return 1;
}
