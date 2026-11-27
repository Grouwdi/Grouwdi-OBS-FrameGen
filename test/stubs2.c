#include <stdlib.h>
#include <stddef.h>
#include <wchar.h>
void os_breakpoint(void) { abort(); }
void bcrash(const char *f, ...) { (void)f; abort(); }
void os_oom(void) { abort(); }
size_t os_mbs_to_utf8_ptr(const char *s, size_t l, char **o) { (void)s; (void)l; *o = NULL; return 0; }
size_t os_utf8_to_wcs_ptr(const char *s, size_t l, wchar_t **o) { (void)s; (void)l; *o = NULL; return 0; }
