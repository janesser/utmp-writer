#
# utmp-writer build
#
#   make            build the binary
#   make install    install to /usr/local (root)
#   make uninstall  remove from /usr/local
#
CC       = gcc
CFLAGS   = -Wall -Wextra -O2
LDFLAGS  =

PREFIX   = /usr/local
BINDIR   = $(PREFIX)/bin
SYSCONFDIR = $(PREFIX)/etc
SYSTEMDDIR = $(PREFIX)/lib/systemd/system

NAME     = utmp-writer
SRCDIR   = src
BUILDDIR = build
BINARY   = $(BUILDDIR)/$(NAME)

.PHONY: all install uninstall clean

all: $(BINARY)

$(BINARY): $(SRCDIR)/utmp-writer.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(BINARY)
	@mkdir -p $(BINDIR)
	@mkdir -p $(SYSTEMDDIR)
	install -m 0755 $(BINARY) $(BINDIR)/$(NAME)
	install -m 0644 etc/systemd/utmp-writer.service \
		$(SYSTEMDDIR)/utmp-writer.service
	@echo "installed $(NAME) to $(PREFIX)"
	@echo "NOTE: add the following line to /etc/pam.d/common-session:"
	@echo "      session optional    pam_exec.so /usr/local/bin/utmp-writer"
	@echo "NOTE: run 'systemctl daemon-reload && systemctl enable --now utmp-writer'"
	@echo "      to activate the boot-time sync service."

uninstall:
	rm -f $(BINDIR)/$(NAME)
	rm -f $(SYSTEMDDIR)/utmp-writer.service
	@echo "uninstalled $(NAME) from $(PREFIX)"
	@echo "NOTE: remove the pam_exec line from /etc/pam.d/common-session manually"

clean:
	rm -f $(BINARY)
