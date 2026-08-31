#ifndef PKG_DPKG_H
#define PKG_DPKG_H

#include "lib/types.h"

/* Minimal dpkg frontend (Phase 4.2): /var/lib/dpkg/status database plus
 * /var/lib/dpkg/info/<pkg>.list file lists. Installs/uninstalls packages
 * unpacked by deb_unpack(); dependency checking verifies that every
 * Depends name is already installed (alternative lists "a | b" accept
 * either; versioned "(>= x)" constraints are recorded but not compared -
 * dependency ORDER is the caller's job (kilget does topological sort). */

/* Install from a .deb already present in the fs (e.g. /var/cache/...).
 * Returns 0 on success; prints progress via term + klog. */
int dpkg_install_file(const char* deb_path);

/* Remove an installed package and its owned files. Returns 0 on success. */
int dpkg_remove(const char* package);

/* Print all installed packages (dpkg -l style short listing). */
void dpkg_list(void);

/* Print files owned by a package; returns 0 if the package exists. */
int dpkg_show_files(const char* package);

/* 1 if the package is installed, 0 otherwise. */
int dpkg_is_installed(const char* package);

/* Transaction mode (like dpkg --force-depends): while set, a missing
 * Depends only warns instead of aborting the install. Needed for real
 * dependency CYCLES (libc6 <-> libgcc-s1): whatever order the planner
 * picks, one member's deps are not installed yet at its turn - the
 * warning stays visible and the final state after the transaction is
 * consistent. Returns the previous flag (nestable). */
int dpkg_set_force_deps(int enable);

#endif
