# utmp-writer

Keep `/run/utmp` populated on Ubuntu systems where systemd does not provide
utmp support.

## Problem

Ubuntu 26.04 ships with systemd 259.9, which is built with `-UTMP`:

- `/run/utmp` is never created
- `systemd-utmcd` is absent
- `pam_utmp.so` is absent

Without a utmp file, `wall`, `who` and `write` have no destination list and
silently deliver nothing. `who` shows only `system boot`, `wall` writes to
nobody.

## Solution

`utmp-writer` maintains `/run/utmp` from PAM login records:

```
/etc/pam.d/common-session:
    session optional    pam_exec.so /usr/local/bin/utmp-writer
```

`pam_exec.so` runs `utmp-writer` on every session open and close. PAM passes
`PAM_USER` / `PAM_TTY` / `PAM_PID` / `PAM_RHOST` / `PAM_TYPE`
(`open_session` / `close_session`) to the executed program, which adds or
removes the utmp entry accordingly.

A oneshot systemd service (`utmp-writer.service`) runs `--sync` at boot so
`/run/utmp` exists and reflects the last-known sessions even if PAM has not
yet run for the current boot. The sync reads the wtmpdb records that
`pam_wtmpdb.so` writes to `/var/log/wtmp.db`.

## Install

```
make && sudo make install
```

This installs:

- `/usr/local/bin/utmp-writer` (the binary)
- `/usr/local/etc/systemd/system/utmp-writer.service` (boot-time sync)
- the `pam_exec.so` line into `/etc/pam.d/common-session`

Then activate:

```
systemctl daemon-reload
systemctl enable --now utmp-writer
```

## Verify

```
who                    # shows logged-in users
wall                   # broadcasts to all login terminals
ls -la /run/utmp       # exists, root-owned, mode 0644
systemctl status utmp-writer
```

## Usage

```
utmp-writer              PAM mode: read PAM_* from the environment
utmp-writer --add USER TTY PID [HOST] [SECONDS]
                         add a utmp entry (default start time = now)
utmp-writer --remove USER TTY
                         remove a utmp entry
utmp-writer --sync       rebuild /run/utmp from wtmpdb records
utmp-writer --create     (re)create /run/utmp, exit
```

## How it works

`pututline()` updates an existing entry with a matching `ut_line` in place,
otherwise appends. Entries for active sessions are `USER_PROCESS` (7);
closed sessions become `DEAD_PROCESS` (8), which `wall()`/`who()` ignore.
The boot record is `BOOT_TIME` (2).

`utmp-writer` is a single static C file with no dependencies beyond the
C library and `libdl` (for the optional SQLite3 lookup). The SQLite3
symbols are loaded dynamically (`dlopen`/`dlsym`), so the package builds
without `libsqlite3-dev` and degrades gracefully when the wtmpdb database
is unavailable.

## Author

jan

## License

MIT
