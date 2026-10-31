/*
PROBE_REPORT.H

The title probes' report: each line goes to the kernel log (klogsrv, port
3232) and to a file in the title's own folder (/app0), which FTP reaches at
/data/homebrew/<TITLE_ID>/. Lines are written as they are made, so a crash
leaves everything up to it.
*/

#ifndef PS5_PROBE_REPORT_H
#define PS5_PROBE_REPORT_H

#include <stdint.h>

/* opens /app0/<name> (made readable and writable by FTP's user) and writes
the console's system software version first */
void report_open(const char *name);
void report_close(void);
void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void heading(const char *text);
/* a notification on the screen, for a person watching the TV */
void notify(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* microseconds from the time stamp counter */
uint64_t now_us(void);

#endif
