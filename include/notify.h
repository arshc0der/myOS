/* notify.h - notifications, calendar events and power/battery information (Control Center, Notification Center). */
#ifndef NOTIFY_H
#define NOTIFY_H
#include "types.h"

#define NOTIFY_MAX 24
struct notification { char app[20], title[40], body[80]; uint32_t uptime_s; uint8_t hour, minute; };

/* Post a notification. It appears as a banner (unless Do Not Disturb is on) and stays in the Notification Center. */
void notify_post(const char *app, const char *title, const char *body);
int  notify_count(void);
const struct notification *notify_get(int i);        /* 0 = newest */
void notify_remove(int i);
void notify_clear(void);
extern volatile uint32_t notify_serial;              /* increases with every post (banner trigger) */

/* Calendar events: stored in /Users/.calendar, one "YYYY-MM-DD text" per line. */
#define CAL_MAX 96
struct cal_event { uint16_t year; uint8_t month, day; char text[44]; };
int  cal_count(void);
const struct cal_event *cal_get(int i);
int  cal_add(int y, int m, int d, const char *text);  /* 0 or -1 */
void cal_remove(int i);
int  cal_on_day(int y, int m, int d, int *idx, int max);   /* indexes of events on that day */
void cal_load(void);
int  cal_days_in_month(int y, int m);
int  cal_weekday(int y, int m, int d);               /* 0 = Sunday */

/* Power: found by reading the ACPI tables. The charge level needs an AML interpreter, which MyOS does not have. */
struct power_info { bool probed, acpi, mobile, battery; };
const struct power_info *power_probe(void);
#endif
