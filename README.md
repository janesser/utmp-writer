# utmp-writer

Make `wall`, `who` and `write` work again on Ubuntu systems where systemd does
not provide `/run/utmp`.

## The problem

Ubuntu 26.04 ships with systemd 259.9, which is built with `-UTMP`:

- `/run/utmp` is never created
- `systemd-utmcd` is absent
- `pam_utmp.so` is absent

`wall`, `who` and `write` get their destination list from `/run/utmp`. Without
it, `who` shows only `system boot`, `wall` writes to nobody and a shutdown
warning (e.g. `auto-poweroff.sh`) never reaches the logged-in sessions.

## What this does

`utmp-writer` maintains `/run/utmp` yourself:

- **PAM** (`pam_exec.so` in `/etc/pam.d/common-session`) adds an entry when a
  session opens and marks it `DEAD_PROCESS` when it closes — real-time, for
  every login, SSH included.
- **systemd** (one-shot `utmp-writer.service`) creates `/run/utmp` + the boot
  record at boot, so the file exists before the first session opens.

After install, `who` shows logged-in users and `wall` broadcasts to all of
them.

## Install

```
make && sudo make install
systemctl daemon-reload
systemctl enable --now utmp-writer
```

This installs `/usr/local/bin/utmp-writer`, the systemd service and appends the
`pam_exec.so` line to `/etc/pam.d/common-session`.

## Usage

```
utmp-writer              PAM mode: read PAM_* from the environment
utmp-writer --add USER TTY PID [HOST] [SECONDS]
                         add a utmp entry (default start time = now)
utmp-writer --remove USER TTY
                         remove a utmp entry
utmp-writer --create     (re)create /run/utmp, exit
```

## Why it works on the next release too

- The program includes `<utmp.h>` and reads `struct utmp` at compile time —
  correct layout even if glibc changes.
- `pam_exec.so` is part of `libpam-modules`, shipped since 1998; the PAM
  environment variables it passes are documented behavior.
- `/run/utmp` is only created when missing; if a future systemd brings back its
  own utmp writer, this service is redundant, never conflicting.

## Author

jan

## License

MIT
