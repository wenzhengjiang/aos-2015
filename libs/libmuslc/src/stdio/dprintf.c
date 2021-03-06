/* @LICENSE(MUSLC_MIT) */

#include <stdio.h>
#include <stdarg.h>

int dprintf(int fd, const char *fmt, ...)
{
	int ret;
	va_list ap;
	va_start(ap, fmt);
	ret = vdprintf(fd, fmt, ap);
	va_end(ap);
	return ret;
}
