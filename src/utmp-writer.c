/*
 * utmp-writer: maintain /run/utmp on systems where systemd does not
 * provide utmp support.
 *
 * Ubuntu 26.04 (systemd 259.9) ships without /run/utmp: systemd is built
 * with -UTMP, systemd-utmcd is absent, pam_utmp.so is absent.  Without a
 * utmp file, `wall`, `who` and `write` have no destination list and
 * silently deliver nothing.
 *
 * This program keeps /run/utmp populated from PAM login records:
 *
 *   /etc/pam.d/common-session:
 *       session optional    pam_exec.so /usr/local/bin/utmp-writer
 *
 * PAM passes PAM_USER / PAM_TTY / PAM_PID / PAM_RHOST / PAM_TYPE
 * (open_session / close_session) to the executed program; utmp entries
 * are added or removed accordingly.
 *
 * A oneshot systemd service (utmp-writer.service) runs --create at boot
 * so /run/utmp exists and carries the boot record even before the first
 * session opens.  PAM keeps the file up to date while sessions run.
 *
 * Usage:
 *   utmp-writer              PAM mode: read PAM_* from the environment
 *   utmp-writer --add USER TTY PID [HOST] [SECONDS]
 *                            add a utmp entry (default start time = now)
 *   utmp-writer --remove USER TTY
 *                            remove a utmp entry
 *   utmp-writer --create     (re)create /run/utmp, exit
 *
 * Author: jan
 * License: MIT
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <utmp.h>

/* The utmp file this program maintains. */
#define UTMP_FILE_PATH "/run/utmp"

/* Permissions for the utmp file: root-owned, readable by all. */
#define UTMP_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

/* Maximum number of utmp entries we will read at once. */
#define UTMP_BATCH 64

/* The utmp_id suffix: last 4 characters of the tty name. */
#define UT_ID_SIZE 4

/*
 * The utmp file location.  Normally UTMP_FILE; override with the UTMP_PATH
 * environment variable (testing only, not for production use).
 */
static const char *
get_utmp_path(void)
{
    const char *env = getenv("UTMP_PATH");
    if (env && env[0] != '\0')
        return env;
    return UTMP_FILE_PATH;
}

/* ------------------------------------------------------------------ */
/*  helpers                                                           */
/* ------------------------------------------------------------------ */

static void
die(const char *msg)
{
    fprintf(stderr, "utmp-writer: %s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

/*
 * Return the last UT_ID_SIZE characters of tty, or the tty itself if it
 * is shorter.  This is the inittab-id convention used by login(1).
 */
static void
utmp_id_from_tty(const char *tty, char *id)
{
    size_t len = strlen(tty);
    if (len > UT_ID_SIZE)
        tty += len - UT_ID_SIZE;
    strncpy(id, tty, UT_ID_SIZE);
    id[UT_ID_SIZE - 1] = '\0';
}

/*
 * Zero a struct utmp so that padding fields are not left uninitialized.
 */
static void
utmp_zero(struct utmp *utmp)
{
    memset(utmp, 0, sizeof *utmp);
}

/*
 * Set the ut_tv field of a struct utmp.  glibc's ut_tv is an anonymous
 * struct (not struct timeval), so it must be set field by field.
 */
static void
utmp_now(struct utmp *utmp)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0)
        die("gettimeofday");
    utmp->ut_tv.tv_sec = tv.tv_sec;
    utmp->ut_tv.tv_usec = tv.tv_usec;
}

/*
 * Create the utmp file if it does not exist.  Root ownership and mode
 * 0644 are required so that wall(1)/who(1) can read it.
 */
static int
create_utmp_file(void)
{
    struct stat st;
    if (stat(get_utmp_path(), &st) == 0)
        return 0; /* already exists */

    int fd = open(get_utmp_path(), O_WRONLY | O_CREAT | O_TRUNC, UTMP_MODE);
    if (fd < 0)
        die("open " UTMP_FILE_PATH);
    if (close(fd) < 0)
        die("close utmp file");
    return 0;
}

/*
 * Write a single entry into the utmp file.  `pututline()` updates an
 * existing entry with a matching ut_line in place, otherwise appends.
 */
static int
write_utmp(struct utmp *utmp)
{
    utmpname(get_utmp_path());
    setutent();
    if (pututline(utmp) == NULL)
    {
        fprintf(stderr, "utmp-writer: pututline failed: %s\n", strerror(errno));
        return -1;
    }
    endutent();
    return 0;
}

/*
 * Write the system boot record into the utmp file (BOOT_TIME).  This is
 * the only entry `who` shows on a machine with no logged-in users.
 *
 * `pututline()` updates an existing entry with a matching ut_line in
 * place, so this is safe to call repeatedly.
 */
static int
write_boot_record(void)
{
    struct utmp utmp;

    create_utmp_file();

    utmp_zero(&utmp);
    utmp.ut_type = BOOT_TIME;
    strncpy(utmp.ut_line, "system boot", UT_LINESIZE);
    utmp.ut_line[UT_LINESIZE - 1] = '\0';
    utmp_now(&utmp);

    return write_utmp(&utmp);
}

/*
 * Read all current entries from the utmp file.  Returns the number of
 * entries read, or -1 on error.  The caller owns `entries`.
 */
static int
read_utmp(struct utmp *entries, int max_entries)
{
    int fd;
    int n = 0;

    fd = open(get_utmp_path(), O_RDONLY);
    if (fd < 0)
        return -1;

    while (n < max_entries && read(fd, &entries[n], sizeof(struct utmp)) ==
                                      sizeof(struct utmp))
        n++;

    close(fd);
    return n;
}

/*
 * Remove the entry for (user, tty) from the utmp file by overwriting it
 * with a DEAD_PROCESS entry.  `wall()` ignores DEAD_PROCESS entries.
 */
static int
remove_utmp_entry(const char *user, const char *tty)
{
    struct utmp entries[UTMP_BATCH];
    int n;
    struct utmp dead;

    n = read_utmp(entries, UTMP_BATCH);
    if (n < 0)
        die("read " UTMP_FILE_PATH);

    for (int i = 0; i < n; i++)
    {
        if (entries[i].ut_type != USER_PROCESS)
            continue;
        if (strcmp(entries[i].ut_user, user) != 0)
            continue;
        if (strcmp(entries[i].ut_line, tty) != 0)
            continue;

        utmp_zero(&dead);
        dead.ut_type = DEAD_PROCESS;
        dead.ut_pid = entries[i].ut_pid;
        strncpy(dead.ut_line, tty, UT_LINESIZE);
        dead.ut_line[UT_LINESIZE - 1] = '\0';
        strncpy(dead.ut_user, user, UT_NAMESIZE);
        dead.ut_user[UT_NAMESIZE - 1] = '\0';
        utmp_now(&dead);

        if (write_utmp(&dead) < 0)
            die("write utmp entry");
        return 0;
    }

    return -1; /* no matching entry found */
}

/* ------------------------------------------------------------------ */
/*  PAM mode                                                          */
/* ------------------------------------------------------------------ */

/*
 * PAM mode: read PAM_USER / PAM_TTY / PAM_PID / PAM_RHOST / PAM_TYPE
 * from the environment and add or remove the utmp entry accordingly.
 *
 * Returns 0 on success, -1 if the PAM environment is unusable.
 */
static int
pam_mode(void)
{
    const char *user = getenv("PAM_USER");
    const char *tty = getenv("PAM_TTY");
    const char *pid = getenv("PAM_PID");
    const char *host = getenv("PAM_RHOST");
    const char *type = getenv("PAM_TYPE");
    struct utmp utmp;
    char id[UT_ID_SIZE];

    if (!user || !tty || !pid || !type)
    {
        fprintf(stderr, "utmp-writer: PAM environment incomplete: "
                        "PAM_USER=%s PAM_TTY=%s PAM_PID=%s PAM_TYPE=%s\n",
                user ? user : "(null)", tty ? tty : "(null)",
                pid ? pid : "(null)", type ? type : "(null)");
        return -1;
    }

    create_utmp_file();

    utmp_zero(&utmp);

    if (strcmp(type, "open_session") == 0)
    {
        /* Session opened: record the login. */
        utmp.ut_type = USER_PROCESS;
        utmp.ut_pid = (pid_t)atoi(pid);
        strncpy(utmp.ut_line, tty, UT_LINESIZE);
        utmp.ut_line[UT_LINESIZE - 1] = '\0';
        utmp_id_from_tty(tty, id);
        strncpy(utmp.ut_id, id, UT_ID_SIZE);
        strncpy(utmp.ut_user, user, UT_NAMESIZE);
        utmp.ut_user[UT_NAMESIZE - 1] = '\0';
        strncpy(utmp.ut_host, host ? host : "local", UT_HOSTSIZE);
        utmp.ut_host[UT_HOSTSIZE - 1] = '\0';
        utmp_now(&utmp);
        utmp.ut_session = utmp.ut_pid;

        if (write_utmp(&utmp) < 0)
            die("write utmp entry");

        fprintf(stderr, "utmp-writer: add %s@%s (pid %s) [%s]\n", user, tty,
                pid, host ? host : "local");
        return 0;
    }

    if (strcmp(type, "close_session") == 0)
    {
        /* Session closed: mark the entry as dead. */
        if (remove_utmp_entry(user, tty) < 0)
            fprintf(stderr, "utmp-writer: no utmp entry for %s@%s to remove\n",
                    user, tty);
        return 0;
    }

    fprintf(stderr, "utmp-writer: unknown PAM_TYPE=%s\n", type);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  --add / --remove                                                  */
/* ------------------------------------------------------------------ */

static int
do_add(int argc, char *argv[])
{
    struct utmp utmp;
    char id[UT_ID_SIZE];

    if (argc < 4)
    {
        fprintf(stderr, "utmp-writer: --add USER TTY PID [HOST] [SECONDS]\n");
        return -1;
    }

    create_utmp_file();

    utmp_zero(&utmp);
    utmp.ut_type = USER_PROCESS;
    utmp.ut_pid = (pid_t)atoi(argv[3]);
    strncpy(utmp.ut_line, argv[2], UT_LINESIZE);
    utmp.ut_line[UT_LINESIZE - 1] = '\0';
    utmp_id_from_tty(argv[2], id);
    strncpy(utmp.ut_id, id, UT_ID_SIZE);
    strncpy(utmp.ut_user, argv[1], UT_NAMESIZE);
    utmp.ut_user[UT_NAMESIZE - 1] = '\0';
    strncpy(utmp.ut_host, argc > 4 ? argv[4] : "local", UT_HOSTSIZE);
    utmp.ut_host[UT_HOSTSIZE - 1] = '\0';
    utmp_now(&utmp);
    utmp.ut_session = utmp.ut_pid;

    if (write_utmp(&utmp) < 0)
        die("write utmp entry");

    fprintf(stderr, "utmp-writer: add %s@%s (pid %s) [%s]\n", argv[1], argv[2],
            argv[3], argc > 4 ? argv[4] : "local");
    return 0;
}

static int
do_remove(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "utmp-writer: --remove USER TTY\n");
        return -1;
    }

    if (remove_utmp_entry(argv[1], argv[2]) < 0)
        fprintf(stderr, "utmp-writer: no utmp entry for %s@%s to remove\n",
                argv[1], argv[2]);
    return 0;
}

static int
do_create(void)
{
    create_utmp_file();
    if (write_boot_record() < 0)
        fprintf(stderr, "utmp-writer: warning: could not write boot record\n");
    fprintf(stderr, "utmp-writer: created " UTMP_FILE_PATH "\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "--add") == 0)
            return do_add(argc - 2, argv + 2);
        if (strcmp(argv[1], "--remove") == 0)
            return do_remove(argc - 2, argv + 2);
        if (strcmp(argv[1], "--create") == 0)
            return do_create();
        fprintf(stderr, "utmp-writer: unknown option %s\n", argv[1]);
        return -1;
    }

    return pam_mode();
}
