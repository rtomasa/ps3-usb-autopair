# SPDX-License-Identifier: MIT

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=

PREFIX ?= /usr
SBINDIR ?= $(PREFIX)/sbin
UDEVDIR ?= $(PREFIX)/lib/udev/rules.d
SYSTEMDDIR ?= $(PREFIX)/lib/systemd/system

TARGET := bin/ps3-usb-autopair
SOURCE := src/ps3-usb-autopair.c
HEADERS := src/sdp_ds3.h
UDEV_RULE := udev/99-ps3-usb-autopair.rules
SYSTEMD_UNIT := systemd/ps3-usb-autopair@.service

.PHONY: all install uninstall clean help

all: $(TARGET)

$(TARGET): $(SOURCE) $(HEADERS)
	install -d "$(dir $@)"
	$(CC) $(CFLAGS) -o "$@" "$(SOURCE)" $(LDFLAGS)

install: $(TARGET)
	install -d "$(DESTDIR)$(SBINDIR)" "$(DESTDIR)$(UDEVDIR)" \
		"$(DESTDIR)$(SYSTEMDDIR)"
	install -m 755 "$(TARGET)" "$(DESTDIR)$(SBINDIR)/ps3-usb-autopair"
	install -m 644 "$(UDEV_RULE)" \
		"$(DESTDIR)$(UDEVDIR)/99-ps3-usb-autopair.rules"
	install -m 644 "$(SYSTEMD_UNIT)" \
		"$(DESTDIR)$(SYSTEMDDIR)/ps3-usb-autopair@.service"

uninstall:
	$(RM) "$(DESTDIR)$(SBINDIR)/ps3-usb-autopair" \
		"$(DESTDIR)$(UDEVDIR)/99-ps3-usb-autopair.rules" \
		"$(DESTDIR)$(SYSTEMDDIR)/ps3-usb-autopair@.service"

clean:
	$(RM) "$(TARGET)"

help:
	@echo "Targets:"
	@echo "  all        Build the pairing helper (default)"
	@echo "  install    Build and install the helper, udev rule, and systemd unit"
	@echo "  uninstall  Remove installed files"
	@echo "  clean      Remove generated files"
	@echo "  help       Show this help"
	@echo
	@echo "Overrides:"
	@echo "  CC=<compiler>       Select the C compiler (default: $(CC))"
	@echo "  CFLAGS=<flags>      Select compiler flags"
	@echo "  PREFIX=<path>       Select the installation prefix (default: $(PREFIX))"
	@echo "  DESTDIR=<path>      Stage installation under this directory"
	@echo "  SBINDIR=<path>      Override the sbin directory"
	@echo "  UDEVDIR=<path>      Override the udev rules directory"
	@echo "  SYSTEMDDIR=<path>   Override the systemd unit directory"
