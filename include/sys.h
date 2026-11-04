/*
 * sys.h - the system layer: standard folders, disks (volumes), user accounts
 * and system preferences (time zone, location, refresh rate).
 *
 * Folder layout (macOS style):
 *     /dev                  devices (null, zero, hda ...)
 *     /Volumes/<disk>      every disk the system could mount
 *     /Users/<name>        home folder: Desktop Documents Downloads Pictures Music
 *     /System               system data   /Applications  (reserved for your programs)
 *     /tmp  /etc  /mnt
 * The first disk that holds a MyFS is mounted on /Users, so accounts and files survive a reboot.
 * Without such a disk /Users lives in RAM and is empty again after a restart.
 */
#ifndef SYS_H
#define SYS_H
#include "types.h"

#define SYS_MAX_USERS 8
#define SYS_NAME_MAX  24

/* ---- standard folders and volumes ---- */
void sys_init(void);                            /* create folders, mount disks (call after ata_init) */
bool sys_users_persistent(void);                /* /Users is on a real disk */
const char *sys_volume_path(const char *dev);   /* where `dev` is mounted, or NULL */
int  sys_volume_mount(const char *dev);         /* mount as MyFS under /Volumes */
int  sys_volume_format(const char *dev);        /* erase + MyFS + mount */

/* ---- user accounts ---- */
struct sys_user { char name[SYS_NAME_MAX]; uint32_t hash; bool admin; };
int  users_count(void);
const struct sys_user *users_get(int i);
int  users_add(const char *name, const char *password, bool admin);   /* 0 or -errno */
int  users_remove(const char *name);
int  users_set_password(const char *name, const char *password);
bool users_check(const char *name, const char *password);
void users_login(const char *name);
const struct sys_user *users_current(void);
const char *users_home(void);                   /* "/Users/arsh" or "/" */
void users_make_home(const char *name);

/* ---- system preferences (saved to /Users/.sysconf when persistent) ---- */
struct sys_prefs {
    int  tz_minutes;                            /* offset from UTC shown on the clock */
    char city[32];                              /* location label                       */
    int  refresh_hz;                            /* desktop frame rate: 30, 60, 75 ...   */
    bool clock24;
};
extern struct sys_prefs sys_prefs;
void sys_prefs_load(void);
void sys_prefs_save(void);
int  sys_place_count(void);
void sys_place_at(int i, const char **city, int *tz_minutes);
void sys_place_set(int i);
void sys_local_time(uint32_t t[6]);             /* rtc time + tz_minutes: y m d H M S */
#endif
