/* sys.c - standard folders, volumes, accounts and preferences (see sys.h). */
#include "sys.h"
#include "vfs.h"
#include "blockdev.h"
#include "kstring.h"
#include "kprintf.h"
#include "kerror.h"
#include "log.h"
#include "hw.h"
#include "fileops.h"
#include "version.h"

static bool persistent;
static int nusers, cur = -1;
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

int sys_disk_candidates(char names[][16], uint32_t mb[], int max)
{
    int n = 0;
    n_found = 0;
    blockdev_for_each(collect, NULL);
    for (int i = 0; i < n_found && n < max; i++) {
        strlcpy(names[n], found[i]->name, 16);
        mb[n] = found[i]->sector_count / 2048;
        n++;
    }
    return n;
}

int sys_use_disk_for_users(const char *dev)
{
    const char *mp = sys_volume_path(dev);
    int r;

    if (persistent)
        return 0;
    if (mp)
        vfs_umount(mp);
    r = vfs_mkfs("myfs", dev);
    if (r < 0)
        return r;
    if (vfs_mount(dev, "/Users", "myfs") < 0)
        return -EIO;
    persistent = true;
    nusers = 0;
    cur = -1;
    sys_prefs_save();
    return 0;
}

int sys_auto_prepare(void)
{
    char names[8][16];
    uint32_t mb[8];
    int n = sys_disk_candidates(names, mb, 8);

    if (persistent)
        return 0;
    for (int i = 0; i < n; i++) {
        struct blockdev *d = blockdev_find(names[i]);
        static uint8_t buf[SECTOR_SIZE * 8];
        bool blank = d && d->sector_count > 64 && blockdev_read(d, 0, 8, buf) == 0;
        for (int k = 0; blank && k < (int)sizeof buf; k++)
            if (buf[k]) blank = false;
        if (blank && sys_use_disk_for_users(names[i]) == 0)
            return 1;
    }
    return 0;
}

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
    char buf[SYS_MAX_USERS * 80 + 8];
    int o = 0;
    for (int i = 0; i < nusers; i++)
        o += ksnprintf(buf + o, sizeof buf - (size_t)o, "%s:%u:%d:%s\n", users[i].name, users[i].hash, users[i].admin, users[i].full);
    vfs_write_file(ACCOUNTS, buf, (size_t)o);
}

static void accounts_load(void)
{
    static char buf[SYS_MAX_USERS * 80 + 8];
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
                char *c3 = strchr(c2, ':');
                if (c3)
                    strlcpy(u->full, c3 + 1, sizeof u->full);
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

void users_set_fullname(const char *name, const char *full)
{
    users_count();
    for (int i = 0; i < nusers; i++)
        if (strcmp(users[i].name, name) == 0) {
            strlcpy(users[i].full, full, sizeof users[i].full);
            for (char *c = users[i].full; *c; c++)
                if (*c == ':' || *c == '\n')
                    *c = ' ';
            accounts_save();
        }
}

void users_logout(void) { cur = -1; }

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

struct sys_prefs sys_prefs = { .tz_minutes = 330, .city = "Kolkata", .refresh_hz = 0, .clock24 = true, .date_format = 0,
    .auto_time = true, .require_password = true, .auto_updates = false, .check_updates = true, .open_terminal = true,
    .volume = 60, .hostname = "myos", .wifi_on = true, .bt_on = true, .brightness = 100 };

const char *const sys_languages[] = { "English", "Hindi", "Spanish", "French", "German", "Portuguese", "Japanese", "Chinese" };
const char *const sys_greetings[] = { "Hello", "Namaste", "Hola", "Bonjour", "Hallo", "Ola", "Konnichiwa", "Ni hao" };
int sys_language_count(void) { return 8; }

const char *const sys_countries[] = { "India", "United States", "United Kingdom", "Canada", "Australia", "Germany", "France",
                                      "Japan", "Singapore", "United Arab Emirates" };
int sys_country_count(void) { return 10; }

void sys_prefs_defaults(void)
{
    struct sys_prefs d = { .tz_minutes = 330, .refresh_hz = 0, .clock24 = true, .auto_time = true, .require_password = true,
                           .check_updates = true, .open_terminal = true, .volume = 60, .wifi_on = true, .bt_on = true, .brightness = 100 };
    strlcpy(d.city, "Kolkata", sizeof d.city);
    strlcpy(d.hostname, "myos", sizeof d.hostname);
    sys_prefs = d;
}

bool sys_hostname_ok(const char *n)
{
    if (!n[0] || strlen(n) >= sizeof sys_prefs.hostname)
        return false;
    for (; *n; n++)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') || (*n >= '0' && *n <= '9') || *n == '-'))
            return false;
    return true;
}

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

void sys_country_apply(int i)
{
    static const struct { const char *city; int fmt; bool h24; } c[] = {
        { "Kolkata", 0, true }, { "New York", 1, false }, { "London", 0, true }, { "Chicago", 1, false },
        { "Sydney", 0, true }, { "Berlin", 0, true }, { "Paris", 0, true }, { "Tokyo", 2, true },
        { "Singapore", 0, true }, { "Dubai", 0, true } };
    if (i < 0 || i >= 10)
        return;
    sys_prefs.country = i;
    sys_prefs.date_format = c[i].fmt;
    sys_prefs.clock24 = c[i].h24;
    for (int k = 0; k < sys_place_count(); k++)
        if (!strcmp(places[k].city, c[i].city))
            sys_place_set(k);
}

#define FLAGS(X) \
    X(setup, setup_done) X(lang, language) X(country, country) X(datefmt, date_format) X(autotime, auto_time) \
    X(acc_contrast, acc_contrast) X(acc_motion, acc_reduce_motion) X(acc_kbd, acc_kbd_pointer) X(acc_reader, acc_reader) \
    X(acc_magnify, acc_magnify) X(priv_diag, priv_diag) X(priv_loc, priv_location) X(priv_ana, priv_analytics) \
    X(reqpw, require_password) X(autoupd, auto_updates) X(chkupd, check_updates) X(voice, voice) X(openterm, open_terminal) \
    X(volume, volume) X(muted, muted) X(dark, dark) X(accent, accent) X(kbdlayout, kbd_layout) X(dnd, dnd) X(airplane, airplane) X(wifion, wifi_on) X(bton, bt_on) X(bright, brightness) X(wallstyle, wall_style) X(wallmode, wall_mode) X(dockhide, dock_hide)

void sys_prefs_save(void)
{
    char b[1400];
    int n = ksnprintf(b, sizeof b, "tz=%d\ncity=%s\nhz=%d\nclock24=%d\nhost=%s\nwall=%s\n", sys_prefs.tz_minutes, sys_prefs.city,
                      sys_prefs.refresh_hz, sys_prefs.clock24, sys_prefs.hostname, sys_prefs.wall);
#define X(key, field) n += ksnprintf(b + n, sizeof b - (size_t)n, #key "=%d\n", (int)sys_prefs.field);
    FLAGS(X)
#undef X
    if (persistent)
        vfs_write_file("/Users/.sysconf", b, (size_t)n);
}

void sys_prefs_load(void)
{
    char b[1500];
    ssize_t n = vfs_read_file("/Users/.sysconf", b, sizeof b - 1);
    if (n <= 0)
        return;
    b[n] = 0;
    for (char *l = b; *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(l, '=');
        if (eq) {
            *eq = 0;
            const char *k = l, *v = eq + 1;
            if (!strcmp(k, "tz")) sys_prefs.tz_minutes = katoi(v);
            else if (!strcmp(k, "city")) strlcpy(sys_prefs.city, v, sizeof sys_prefs.city);
            else if (!strcmp(k, "hz")) sys_prefs.refresh_hz = katoi(v);
            else if (!strcmp(k, "clock24")) sys_prefs.clock24 = katoi(v) != 0;
            else if (!strcmp(k, "host")) strlcpy(sys_prefs.hostname, v, sizeof sys_prefs.hostname);
            else if (!strcmp(k, "wall")) strlcpy(sys_prefs.wall, v, sizeof sys_prefs.wall);
#define X(key, field) else if (!strcmp(k, #key)) sys_prefs.field = (typeof(sys_prefs.field))katoi(v);
            FLAGS(X)
#undef X
        }
        if (!nl) break;
        l = nl + 1;
    }
}

void sys_factory_reset(bool erase_data)
{
    struct dirent e;

    if (erase_data) {
        vfs_unlink(ACCOUNTS);
        for (int guard = 0; guard < 64 && vfs_readdir("/Users", 0, &e) == 0; guard++) {
            char p[VFS_PATH_MAX];
            ksnprintf(p, sizeof p, "/Users/%s", e.name);
            if (fo_remove(p) < 0)
                break;
        }
        nusers = 0;
        cur = -1;
    }
    sys_prefs_defaults();                       /* setup_done = false */
    sys_prefs_save();
    vfs_unlink("/Users/.sysconf");
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

void sys_date_string(char *out, size_t n)
{
    uint32_t r[6];
    rtc_read(r);
    if (sys_prefs.date_format == 1)
        ksnprintf(out, n, "%02u/%02u/%04u", r[1], r[2], r[0]);
    else if (sys_prefs.date_format == 2)
        ksnprintf(out, n, "%04u-%02u-%02u", r[0], r[1], r[2]);
    else
        ksnprintf(out, n, "%02u/%02u/%04u", r[2], r[1], r[0]);
}

/* ------------------------------------------------------------------ */
/* Translations                                                        */
/* ------------------------------------------------------------------ */

static const char *const tr_table[][6] = {
    /* English, Hindi (Latin), Spanish, French, German, Portuguese */
    { "Back", "Peeche", "Atras", "Retour", "Zurueck", "Voltar" },
    { "Continue", "Aage badhein", "Continuar", "Continuer", "Weiter", "Continuar" },
    { "Skip", "Chhodein", "Omitir", "Passer", "Ueberspringen", "Pular" },
    { "Finish", "Poora karein", "Terminar", "Terminer", "Fertig", "Concluir" },
    { "Start Setup", "Setup shuru karein", "Iniciar", "Demarrer", "Einrichten", "Iniciar" },
    { "Quick Setup", "Tez Setup", "Rapido", "Rapide", "Schnell", "Rapido" },
    { "Get Started", "Shuru karein", "Empezar", "Commencer", "Los geht's", "Comecar" },
    { "Log In", "Login karein", "Entrar", "Connexion", "Anmelden", "Entrar" },
    { "Restart", "Dobara chalu", "Reiniciar", "Redemarrer", "Neustart", "Reiniciar" },
    { "Shut Down", "Band karein", "Apagar", "Eteindre", "Ausschalten", "Desligar" },
    { "Welcome", "Swagat hai", "Bienvenido", "Bienvenue", "Willkommen", "Bem-vindo" },
    { "Hardware check", "Hardware jaanch", "Revision del equipo", "Verification materiel", "Hardware-Pruefung", "Verificacao" },
    { "Storage", "Storage", "Almacenamiento", "Stockage", "Speicher", "Armazenamento" },
    { "Language", "Bhasha", "Idioma", "Langue", "Sprache", "Idioma" },
    { "Country or region", "Desh ya kshetra", "Pais o region", "Pays ou region", "Land oder Region", "Pais ou regiao" },
    { "Accessibility", "Sugamta", "Accesibilidad", "Accessibilite", "Bedienungshilfen", "Acessibilidade" },
    { "Keyboard", "Keyboard", "Teclado", "Clavier", "Tastatur", "Teclado" },
    { "Network", "Network", "Red", "Reseau", "Netzwerk", "Rede" },
    { "Data and privacy", "Data aur privacy", "Datos y privacidad", "Donnees et confidentialite", "Daten und Datenschutz", "Dados e privacidade" },
    { "Create your account", "Apna account banayein", "Crea tu cuenta", "Creez votre compte", "Konto erstellen", "Crie sua conta" },
    { "Computer name", "Computer ka naam", "Nombre del equipo", "Nom de l'ordinateur", "Computername", "Nome do computador" },
    { "Date and time", "Tarikh aur samay", "Fecha y hora", "Date et heure", "Datum und Uhrzeit", "Data e hora" },
    { "Final preferences", "Antim pasand", "Preferencias finales", "Preferences finales", "Letzte Einstellungen", "Preferencias finais" },
    { "Appearance", "Roop", "Apariencia", "Apparence", "Darstellung", "Aparencia" },
    { "Dock", "Dock", "Dock", "Dock", "Dock", "Dock" },
    { "Pointer", "Pointer", "Puntero", "Pointeur", "Zeiger", "Ponteiro" },
    { "Display", "Display", "Pantalla", "Ecran", "Bildschirm", "Tela" },
    { "Date & Time", "Tarikh aur samay", "Fecha y hora", "Date et heure", "Datum & Zeit", "Data e hora" },
    { "Users", "Upyogkarta", "Usuarios", "Utilisateurs", "Benutzer", "Usuarios" },
    { "Devices", "Devices", "Dispositivos", "Peripheriques", "Geraete", "Dispositivos" },
    { "Sound", "Dhwani", "Sonido", "Son", "Ton", "Som" },
    { "Privacy", "Privacy", "Privacidad", "Confidentialite", "Datenschutz", "Privacidade" },
    { "General", "Aam", "General", "General", "Allgemein", "Geral" },
    { "Hardware", "Hardware", "Equipo", "Materiel", "Hardware", "Hardware" },
    { "Enter your password", "Password daalein", "Escribe tu contrasena", "Entrez votre mot de passe", "Passwort eingeben", "Digite a senha" },
    { "Wrong password. Try again.", "Galat password. Phir koshish karein.", "Contrasena incorrecta.", "Mot de passe incorrect.", "Falsches Passwort.", "Senha incorreta." },
    { "Setting up your computer", "Computer taiyaar ho raha hai", "Preparando tu equipo", "Configuration en cours", "Computer wird eingerichtet", "Preparando o computador" },
    { "Your computer is ready.", "Aapka computer taiyaar hai.", "Tu equipo esta listo.", "Votre ordinateur est pret.", "Dein Computer ist bereit.", "Seu computador esta pronto." },
};

const char *sys_tr(const char *en)
{
    int l = sys_prefs.language;
    if (l < 1 || l > 5)
        return en;
    for (unsigned i = 0; i < sizeof tr_table / sizeof *tr_table; i++)
        if (!strcmp(tr_table[i][0], en))
            return tr_table[i][l];
    return en;
}

/* ------------------------------------------------------------------ */
/* Updates from a disk                                                 */
/* ------------------------------------------------------------------ */

static uint32_t ver_num(const char *v)
{
    uint32_t p[3] = { 0, 0, 0 };
    for (int i = 0; i < 3 && *v; i++) {
        while (*v >= '0' && *v <= '9') p[i] = p[i] * 10 + (uint32_t)(*v++ - '0');
        if (*v == '.') v++;
    }
    return p[0] * 10000 + p[1] * 100 + p[2];
}

int sys_update_check(char *msg, size_t n)
{
    struct dirent e;
    char path[VFS_PATH_MAX], v[24];

    for (uint32_t i = 0; vfs_readdir("/Volumes", i, &e) == 0; i++) {
        ksnprintf(path, sizeof path, "/Volumes/%s/MyOSUpdate/VERSION", e.name);
        ssize_t r = vfs_read_file(path, v, sizeof v - 1);
        if (r <= 0)
            continue;
        v[r] = 0;
        for (char *c = v; *c; c++)
            if (*c == '\n' || *c == '\r') *c = 0;
        if (ver_num(v) > ver_num(MYOS_VERSION)) {
            ksnprintf(msg, n, "MyOS %s is on %s (MyOSUpdate). Put its kernel.elf on your boot disk and restart.", v, e.name);
            return 1;
        }
    }
    ksnprintf(msg, n, "MyOS %s is up to date. Updates are looked for in a MyOSUpdate folder on connected disks.", MYOS_VERSION);
    return 0;
}
