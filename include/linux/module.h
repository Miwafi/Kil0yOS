/*
 * linux/module.h - built-in driver stubs
 *
 * Linux drivers are compiled directly into the kernel image, so all
 * module plumbing collapses to empty macros. Kconfig symbols the
 * drivers expect are passed on the command line instead.
 */
#ifndef _COMPAT_LINUX_MODULE_H
#define _COMPAT_LINUX_MODULE_H

#define MODULE_LICENSE(lic)       static const char __module_license[] = lic
#define MODULE_AUTHOR(a)          static const char __module_author[] = a
#define MODULE_DESCRIPTION(d)     static const char __module_desc[] = d
#define MODULE_VERSION(v)         static const char __module_ver[] = v
#define MODULE_ALIAS(a)
#define MODULE_SUPPORTED_DEVICE(d)
#define MODULE_PARM_DESC(p, d)
#define MODULE_DEVICE_TABLE(bus, table) \
    static const void* __module_device_table_##bus __attribute__((unused)) = table

#define module_param(name, type, perm)
#define module_param_named(name, val, type, perm)
#define module_param_array(name, type, nump, perm)

/* Built-in init: drivers drop an initcall into .compat_initcall via
 * module_init(); compat_initcalls() (called after PCI enumeration)
 * runs them in link order. Multiple drivers may use module_init(). */
struct compat_initcall {
    int (*fn)(void);
    const char* name;
};

#define module_init(fn)                                                  \
    static const struct compat_initcall __compat_ic_##fn                 \
    __attribute__((section(".compat_initcall"), used)) = { fn, #fn }
#define module_exit(fn)
#define __init
#define __exit
#define __devinit
#define __devexit
#define __devinitdata
#define __devexit_p(x) x
#define __initdata
#define __read_mostly
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define THIS_MODULE ((struct module*)0)
struct module;

#define MODULE_NAME "compat"

/* pr_fmt() users reference KBUILD_MODNAME; the build passes the real
 * name per translation unit, this is only a fallback. */
#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "kmod"
#endif

#endif /* _COMPAT_LINUX_MODULE_H */
