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
bool sys_users_persistent(void);
/* Disks a user could keep /Users on (not RAM disks). Returns count. */
int  sys_disk_candidates(char names[][16], uint32_t mb[], int max);
/* Erase `dev`, make a MyFS on it and use it for /Users so settings and accounts survive restarts. */
int  sys_use_disk_for_users(const char *dev);
/* Same for the first completely blank disk (used by Quick Setup). 1 if done. */
int  sys_auto_prepare(void);                /* /Users is on a real disk */
const char *sys_volume_path(const char *dev);   /* where `dev` is mounted, or NULL */
int  sys_volume_mount(const char *dev);         /* mount as MyFS under /Volumes */
int  sys_volume_format(const char *dev);        /* erase + MyFS + mount */

/* ---- user accounts ---- */
struct sys_user { char name[SYS_NAME_MAX]; uint32_t hash; bool admin; char full[32]; };
int  users_count(void);
const struct sys_user *users_get(int i);
int  users_add(const char *name, const char *password, bool admin);   /* 0 or -errno */
void users_set_fullname(const char *name, const char *full);
void users_logout(void);
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
    /* set by the Setup Assistant, editable in System Settings */
    bool setup_done;                            /* first-boot setup completed           */
    int  language;                              /* index into sys_languages             */
    int  country;                               /* index into sys_countries             */
    int  date_format;                           /* 0 DD/MM/YYYY, 1 MM/DD/YYYY, 2 YYYY-MM-DD */
    bool auto_time;                             /* keep time from the hardware clock    */
    bool acc_contrast, acc_reduce_motion, acc_kbd_pointer, acc_reader, acc_magnify;
    bool priv_diag, priv_location, priv_analytics, require_password;
    bool auto_updates, check_updates, voice, open_terminal;
    int  volume;                                /* 0..100 (no sound driver yet: stored only) */
    bool muted;
    char hostname[24];
    bool dark;                                  /* light / dark appearance (mirrors gui_prefs.dark) */
    int  accent;                                /* accent colour index                  */
    int  kbd_layout;                            /* 0 US, 1 UK, 2 German, 3 French, 4 Hindi (saved; key translation is US only) */
};
extern struct sys_prefs sys_prefs;
void sys_prefs_load(void);
void sys_prefs_save(void);
int  sys_place_count(void);
void sys_place_at(int i, const char **city, int *tz_minutes);
void sys_place_set(int i);
void sys_local_time(uint32_t t[6]);
void sys_date_string(char *out, size_t n);       /* in the chosen date format */
extern const char *const sys_languages[];
int  sys_language_count(void);
extern const char *const sys_greetings[];      /* "Hello", "Namaste" ... same order as languages */
extern const char *const sys_countries[];
int  sys_country_count(void);
void sys_country_apply(int i);                  /* sets the time zone / city / date format for a country */
bool sys_hostname_ok(const char *n);
/* Erase settings (and with erase_data also accounts and home folders), mark setup as not done. */
void sys_factory_reset(bool erase_data);
void sys_prefs_defaults(void);
/* Text in the chosen language (English if there is no translation). Languages: hi (Latin letters), es, fr, de, pt. */
const char *sys_tr(const char *english);
/* Look for a newer MyOS on the mounted disks (folder MyOSUpdate with a VERSION file). Returns 1 if found;
 * `msg` always receives a sentence for the user. */
int  sys_update_check(char *msg, size_t n);             /* rtc time + tz_minutes: y m d H M S */
#endif
