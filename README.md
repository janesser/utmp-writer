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

A oneshot systemd service (`utmp-writer.service`) runs `--create` at boot so
`/run/utmp` exists and carries the boot record even before the first session
opens.  PAM keeps the file up to date while sessions run.

## Install

```
make && sudo make install
```

This installs:

- `/usr/local/bin/utmp-writer` (the binary)
- `/usr/local/lib/systemd/system/utmp-writer.service` (boot-time record)
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
utmp-writer --create     (re)create /run/utmp, exit
```

## How it works

`pututline()` updates an existing entry with a matching `ut_line` in place,
otherwise appends. Entries for active sessions are `USER_PROCESS` (7);
closed sessions become `DEAD_PROCESS` (8), which `wall()`/`who()` ignore.
The boot record is `BOOT_TIME` (2).

The program is a single C file with **no dependencies beyond the C library**.
It includes `<utmp.h>` and reads `struct utmp` at compile time, so it stays
correct when glibc changes the layout.

## Author

jan

## License

MIT
