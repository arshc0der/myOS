#include "notify.h"
#include "sys.h"
#include "timer.h"
#include "vfs.h"
#include "kstring.h"
#include "kprintf.h"

volatile uint32_t notify_serial;
static struct notification list[NOTIFY_MAX];
static int n;

void notify_post(const char *app, const char *title, const char *body)
{
    uint32_t t[6];
    if (n == NOTIFY_MAX)
        n--;
    for (int i = n; i > 0; i--)
        list[i] = list[i - 1];
    n++;
    strlcpy(list[0].app, app ? app : "System", sizeof list[0].app);
    strlcpy(list[0].title, title ? title : "", sizeof list[0].title);
    strlcpy(list[0].body, body ? body : "", sizeof list[0].body);
    sys_local_time(t);
    list[0].hour = (uint8_t)t[3];
    list[0].minute = (uint8_t)t[4];
    list[0].uptime_s = timer_uptime_sec();
    notify_serial++;
}

int notify_count(void) { return n; }
const struct notification *notify_get(int i) { return i >= 0 && i < n ? &list[i] : NULL; }

void notify_remove(int i)
{
    if (i < 0 || i >= n)
        return;
    for (; i + 1 < n; i++)
        list[i] = list[i + 1];
    n--;
}

void notify_clear(void) { n = 0; }

/* ---- calendar ---- */

static struct cal_event ev[CAL_MAX];
static int nev;
static bool loaded;

int cal_days_in_month(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[(m - 1) % 12];
}

int cal_weekday(int y, int m, int d)               /* Sakamoto's method */
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void cal_save(void)
{
    static char buf[CAL_MAX * 60];
    int o = 0;
    for (int i = 0; i < nev; i++)
        o += ksnprintf(buf + o, sizeof buf - (size_t)o, "%04d-%02d-%02d %s\n", ev[i].year, ev[i].month, ev[i].day, ev[i].text);
    if (sys_users_persistent())
        vfs_write_file("/Users/.calendar", buf, (size_t)o);
}

void cal_load(void)
{
    static char buf[CAL_MAX * 60];
    ssize_t r;

    loaded = true;
    nev = 0;
    r = vfs_read_file("/Users/.calendar", buf, sizeof buf - 1);
    if (r <= 0)
        return;
    buf[r] = 0;
    for (char *l = buf; *l && nev < CAL_MAX;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (strlen(l) > 11 && l[4] == '-' && l[7] == '-') {
            ev[nev].year = (uint16_t)katoi(l);
            ev[nev].month = (uint8_t)katoi(l + 5);
            ev[nev].day = (uint8_t)katoi(l + 8);
            strlcpy(ev[nev].text, l + 11, sizeof ev[nev].text);
            nev++;
        }
        if (!nl) break;
        l = nl + 1;
    }
}

int cal_count(void) { if (!loaded) cal_load(); return nev; }
const struct cal_event *cal_get(int i) { cal_count(); return i >= 0 && i < nev ? &ev[i] : NULL; }

int cal_add(int y, int m, int d, const char *text)
{
    cal_count();
    if (nev >= CAL_MAX || !text[0])
        return -1;
    ev[nev].year = (uint16_t)y;
    ev[nev].month = (uint8_t)m;
    ev[nev].day = (uint8_t)d;
    strlcpy(ev[nev].text, text, sizeof ev[nev].text);
    nev++;
    cal_save();
    return 0;
}

void cal_remove(int i)
{
    cal_count();
    if (i < 0 || i >= nev)
        return;
    for (; i + 1 < nev; i++)
        ev[i] = ev[i + 1];
    nev--;
    cal_save();
}

int cal_on_day(int y, int m, int d, int *idx, int max)
{
    int k = 0;
    cal_count();
    for (int i = 0; i < nev && k < max; i++)
        if (ev[i].year == y && ev[i].month == m && ev[i].day == d)
            idx[k++] = i;
    return k;
}
