#ifndef PKG_KILGET_H
#define PKG_KILGET_H

#include "lib/types.h"

/* kilget - the Phase 4.4/4.5 repo client and apt-get-equivalent frontend.
 *
 *   kilget update        download the Packages index (flat repo layout)
 *   kilget install <pkg> resolve dependencies (topological order),
 *                        download .debs to /var/cache/kilget, verify
 *                        SHA256 against the index, install via dpkg
 *   kilget remove <pkg>  dpkg remove
 *   kilget show <pkg>    index metadata for one package
 *   kilget list          available packages in the index
 *   kilget installed     dpkg -l equivalent
 *
 * Source definition comes from /etc/kilget/sources.list (one line):
 *   deb http://kil0yos:8000/krepo ./
 */

int  kilget_update(void);
int  kilget_install(const char* package);
void kilget_show(const char* package);
void kilget_list(void);

#endif
