/* rtld-audit shim: log every library search the dynamic linker performs and
 * deny (return NULL) the ones matching the regex in $AUDIT_DENY. */
#define _GNU_SOURCE
#include <link.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static regex_t deny; static int have_deny;
unsigned int la_version(unsigned int v) { const char *d = getenv("AUDIT_DENY"); if (d && *d) have_deny = regcomp(&deny, d, REG_EXTENDED|REG_NOSUB) == 0; return LAV_CURRENT; }
char *la_objsearch(const char *name, uintptr_t *cookie, unsigned int flag) {
  if (flag != LA_SER_ORIG) return (char *)name;
  int d = have_deny && regexec(&deny, name, 0, NULL, 0) == 0;
  fprintf(stderr, "AUDIT: search %s%s\n", name, d ? "  [DENIED]" : "");
  return d ? NULL : (char *)name;
}
unsigned int la_objopen(struct link_map *m, Lmid_t l, uintptr_t *c) { if (m->l_name && *m->l_name) fprintf(stderr, "AUDIT: opened %s\n", m->l_name); return 0; }
