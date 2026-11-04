/* sys.c - standard folders, volumes, accounts and preferences (see sys.h). */
#include "sys.h"
#include "vfs.h"
#include "blockdev.h"
#include "kstring.h"
#include "kprintf.h"
#include "kerror.h"
#include "log.h"
#include "hw.h"

static bool persistent;
static struct blockdev *found[16];
static int n_found;

static void collect(struct blockdev *d, void *c)
{
    (void)c;
    if (n_found < 16 && strcmp(d->kind, "ram") != 0)
        found[n_found++] = d;
}

/* ------------------------------------------------------------------ */
/* Folders and volumes                                                 */
/* ------------------------------------------------------------------ */

static void mk(const char *p)
{
    struct vfs_stat st;
    if (vfs_stat(p, &st) < 0)
        vfs_mkdir(p);
}

struct find_ctx { const char *dev; char path[VFS_PATH_MAX]; };

static void find_mount(const struct vfs_mount *m, void *c)
{
    struct find_ctx *f = c;
    if (m->dev && strcmp(m->dev->name, f->dev) == 0 && !f->path[0])
        strlcpy(f->path, m->path, sizeof f->path);
}

const char *sys_volume_path(const char *dev)
{
    static struct find_ctx f;
    f.dev = dev;
    f.path[0] = 0;
    vfs_for_each_mount(find_mount, &f);
    return f.path[0] ? f.path : NULL;
}

int sys_volume_mount(const char *dev)
{
    char p[VFS_PATH_MAX];
    if (sys_volume_path(dev))
        return 0;
    ksnprintf(p, sizeof p, "/Volumes/%s", dev);
    mk(p);
    int r = vfs_mount(dev, p, "myfs");
    if (r < 0)
        vfs_unlink(p);
    return r;
}

int sys_volume_format(const char *dev)
{
    const char *mp = sys_volume_path(dev);
    if (mp)
        vfs_umount(mp);
    int r = vfs_mkfs("myfs", dev);
    if (r < 0)
        return r;
    return sys_volume_mount(dev);
}

bool sys_users_persistent(void) { return persistent; }

void sys_init(void)
{
    static const char *dirs[] = { "/Volumes", "/Users", "/System", "/Applications", "/tmp", "/etc", "/mnt" };
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++)
        mk(dirs[i]);

    char names[16][16];
    n_found = 0;
    blockdev_for_each(collect, NULL);
    int n = n_found;
    for (int i = 0; i < n; i++)
        strlcpy(names[i], found[i]->name, sizeof names[i]);

    for (int i = 0; i < n; i++) {
        if (!persistent) {                      /* the first MyFS disk becomes /Users */
            if (vfs_mount(names[i], "/Users", "myfs") == 0) {
                persistent = true;
                log_info("sys", "%s holds the user folders (/Users)", names[i]);
                continue;
            }
        }
        if (sys_volume_mount(names[i]) == 0)
            log_info("sys", "%s mounted on /Volumes/%s", names[i], names[i]);
    }
    sys_prefs_load();
}

/* ------------------------------------------------------------------ */
/* Accounts                                                            */
/* ------------------------------------------------------------------ */

#define ACCOUNTS "/Users/.accounts"
static struct sys_user users[SYS_MAX_USERS];
static int nusers;
static int cur = -1;

/* FNV-1a with a fixed salt. This keeps passwords out of plain sight; it is a teaching
 * example, not strong security. */
static uint32_t pw_hash(const char *name, const char *pw)
{
    uint32_t h = 2166136261u;
    for (const char *p = "myos$"; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (const char *p = pw; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static void accounts_save(void)
{
    char buf[SYS_MAX_USERS * 48 + 8];
    int o = 0;
    for (int i = 0; i < nusers; i++)
        o += ksnprintf(buf + o, sizeof buf - (size_t)o, "%s:%u:%d\n", users[i].name, users[i].hash, users[i].admin);
    vfs_write_file(ACCOUNTS, buf, (size_t)o);
}

static void accounts_load(void)
{
    static char buf[SYS_MAX_USERS * 48 + 8];
    nusers = 0;
    ssize_t n = vfs_read_file(ACCOUNTS, buf, sizeof buf - 1);
    if (n <= 0)
        return;
    buf[n] = 0;
    for (char *line = buf; *line && nusers < SYS_MAX_USERS;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *c1 = strchr(line, ':');
        if (c1) {
            *c1++ = 0;
            char *c2 = strchr(c1, ':');
            if (c2) {
                *c2++ = 0;
                struct sys_user *u = &users[nusers++];
                strlcpy(u->name, line, sizeof u->name);
                uint32_t h = 0;
                for (const char *p = c1; *p >= '0' && *p <= '9'; p++) h = h * 10 + (uint32_t)(*p - '0');
                u->hash = h;
                u->admin = *c2 == '1';
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
}

int users_count(void)
{
    static bool loaded;
    if (!loaded) { loaded = true; accounts_load(); }
    return nusers;
}
const struct sys_user *users_get(int i) { users_count(); return i >= 0 && i < nusers ? &users[i] : NULL; }

void users_make_home(const char *name)
{
    static const char *sub[] = { "Desktop", "Documents", "Downloads", "Pictures", "Music" };
    char p[VFS_PATH_MAX];
    ksnprintf(p, sizeof p, "/Users/%s", name);
    mk(p);
    for (unsigned i = 0; i < sizeof sub / sizeof *sub; i++) {
        ksnprintf(p, sizeof p, "/Users/%s/%s", name, sub[i]);
        mk(p);
    }
}

static bool name_ok(const char *n)
{
    if (!n[0] || strlen(n) >= SYS_NAME_MAX)
        return false;
    for (; *n; n++)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') || (*n >= '0' && *n <= '9') || *n == '_' || *n == '-' || *n == '.'))
            return false;
    return true;
}

int users_add(const char *name, const char *password, bool admin)
{
    users_count();
    if (!name_ok(name))
        return -EINVAL;
    for (int i = 0; i < nusers; i++)
        if (strcmp(users[i].name, name) == 0)
            return -EEXIST;
    if (nusers >= SYS_MAX_USERS)
        return -ENOSPC;
    struct sys_user *u = &users[nusers++];
    strlcpy(u->name, name, sizeof u->name);
    u->hash = pw_hash(name, password);
    u->admin = admin;
    users_make_home(name);
    accounts_save();
    return 0;
}

int users_remove(const char *name)
{
    users_count();
    for (int i = 0; i < nusers; i++)
        if (strcmp(users[i].name, name) == 0) {
            if (i == cur) return -EBUSY;
            for (int j = i; j + 1 < nusers; j++) users[j] = users[j + 1];
            nusers--;
            if (cur > i) cur--;
            accounts_save();
            return 0;
        }
    return -ENOENT;
}

int users_set_password(const char *name, const char *password)
{
    users_count();
    for (int i = 0; i < nusers; i++)
        if (strcmp(users[i].name, name) == 0) {
            users[i].hash = pw_hash(name, password);
            accounts_save();
            return 0;
        }
    return -ENOENT;
}

bool users_check(const char *name, const char *password)
{
    users_count();
    for (int i = 0; i < nusers; i++)
        if (strcmp(users[i].name, name) == 0)
            return users[i].hash == pw_hash(name, password);
    return false;
}

void users_login(const char *name)
{
    users_count();
    for (int i = 0; i < nusers; i++)
        if (strcmp(users[i].name, name) == 0) {
            cur = i;
            users_make_home(name);
            vfs_chdir(users_home());
            return;
        }
}

const struct sys_user *users_current(void) { return cur >= 0 ? &users[cur] : NULL; }

const char *users_home(void)
{
    static char h[48];
    if (cur < 0)
        return "/";
    ksnprintf(h, sizeof h, "/Users/%s", users[cur].name);
    return h;
}

/* ------------------------------------------------------------------ */
/* Preferences: time zone, location, refresh rate                      */
/* ------------------------------------------------------------------ */

struct sys_prefs sys_prefs = { .tz_minutes = 330, .city = "Kolkata", .refresh_hz = 60, .clock24 = true };

static const struct { const char *city; int tz; } places[] = {
    { "Kolkata", 330 }, { "Delhi", 330 }, { "Mumbai", 330 }, { "Dubai", 240 }, { "London", 0 },
    { "Paris", 60 }, { "Berlin", 60 }, { "Moscow", 180 }, { "Singapore", 480 }, { "Tokyo", 540 },
    { "Sydney", 600 }, { "New York", -300 }, { "Chicago", -360 }, { "Los Angeles", -480 }, { "UTC", 0 },
};

int sys_place_count(void) { return (int)(sizeof places / sizeof *places); }
void sys_place_at(int i, const char **city, int *tz) { *city = places[i].city; *tz = places[i].tz; }
void sys_place_set(int i)
{
    strlcpy(sys_prefs.city, places[i].city, sizeof sys_prefs.city);
    sys_prefs.tz_minutes = places[i].tz;
    sys_prefs_save();
}

void sys_prefs_save(void)
{
    char b[128];
    int n = ksnprintf(b, sizeof b, "tz=%d\ncity=%s\nhz=%d\nclock24=%d\n", sys_prefs.tz_minutes, sys_prefs.city,
                      sys_prefs.refresh_hz, sys_prefs.clock24);
    if (persistent)
        vfs_write_file("/Users/.sysconf", b, (size_t)n);
}

void sys_prefs_load(void)
{
    char b[160];
    ssize_t n = vfs_read_file("/Users/.sysconf", b, sizeof b - 1);
    if (n <= 0)
        return;
    b[n] = 0;
    for (char *l = b; *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (!strncmp(l, "tz=", 3)) sys_prefs.tz_minutes = katoi(l + 3);
        else if (!strncmp(l, "city=", 5)) strlcpy(sys_prefs.city, l + 5, sizeof sys_prefs.city);
        else if (!strncmp(l, "hz=", 3)) sys_prefs.refresh_hz = katoi(l + 3);
        else if (!strncmp(l, "clock24=", 8)) sys_prefs.clock24 = katoi(l + 8) != 0;
        if (!nl) break;
        l = nl + 1;
    }
}

void sys_local_time(uint32_t t[6])
{
    uint32_t r[6];
    rtc_read(r);                                /* the RTC is treated as UTC */
    for (int i = 0; i < 6; i++)
        t[i] = r[i];
    int total = ((int)r[3] * 60 + (int)r[4] + sys_prefs.tz_minutes) % 1440;
    if (total < 0)
        total += 1440;
    t[3] = (uint32_t)(total / 60);              /* date is not adjusted across midnight */
    t[4] = (uint32_t)(total % 60);
}
