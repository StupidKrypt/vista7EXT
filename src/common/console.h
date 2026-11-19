#ifndef VX_CONSOLE_H
#define VX_CONSOLE_H
void con_write(const WCHAR *s);
void con_print(const WCHAR *fmt, ...);
void con_log(void *ctx, const WCHAR *line);
#endif
