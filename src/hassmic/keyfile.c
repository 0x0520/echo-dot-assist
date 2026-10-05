/* Key files in the state directory, see keyfile.h. */
#include "keyfile.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static void path_of(char *p, size_t n, const char *name)
{
    const char *d = getenv("HASSMIC_STATE");
    snprintf(p, n, "%s/%s", d ? d : "/data/local/hassmic/state", name);
}

FILE *keyfile_read(const char *name)
{
    char p[256]; path_of(p, sizeof p, name);
    return fopen(p, "r");
}

FILE *keyfile_write(struct keyfile *k, const char *name, const char *tag)
{
    k->tag = tag; path_of(k->path, sizeof k->path, name);
    snprintf(k->tmp, sizeof k->tmp, "%s.tmp", k->path);
    int fd = open(k->tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    k->f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!k->f) { if (fd >= 0) close(fd); fprintf(stderr, "%s: cannot write %s\n", tag, k->tmp); }
    return k->f;
}

void keyfile_commit(struct keyfile *k)
{
    if (fclose(k->f) || rename(k->tmp, k->path)) { unlink(k->tmp); fprintf(stderr, "%s: cannot write %s\n", k->tag, k->path); }
    k->f = NULL;
}
