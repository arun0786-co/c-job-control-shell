#include "shell.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char user[256] = "user";
static char host[256] = "host";

void prompt_init(void)
{
    struct passwd *pw = getpwuid(getuid());
    const char *name = pw ? pw->pw_name : getenv("USER");
    if (name != NULL)
        snprintf(user, sizeof user, "%s", name);
    if (gethostname(host, sizeof host) == 0)
        host[sizeof host - 1] = '\0'; /* not terminated if it was cut short */
}

/* <user@host:path>, with the shell's home directory shown as ~ */
char *prompt_text(void)
{
    const char *home = shell_home();
    size_t n = strlen(home);
    char *cwd = getcwd(NULL, 0);
    const char *shown = cwd ? cwd : "?";
    const char *tilde = "";
    if (cwd != NULL && strncmp(cwd, home, n) == 0 && (cwd[n] == '\0' || cwd[n] == '/')) {
        shown = cwd + n;
        tilde = "~";
    }
    size_t size = strlen(user) + strlen(host) + strlen(shown) + 8;
    char *text = xrealloc(NULL, size);
    snprintf(text, size, "<%s@%s:%s%s> ", user, host, tilde, shown);
    free(cwd);
    return text;
}
