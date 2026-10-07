# UltraInit 1.2.0 - Makefile
# gcc/clang ile derlenir (Linux).

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
LDFLAGS ?=

PREFIX      ?= /usr
SBINDIR     := $(PREFIX)/sbin
BINDIR      := $(PREFIX)/bin
CONFDIR     := /etc/ultrainit
INITDDIR    := $(CONFDIR)/init.d
UNITSDIR    := $(CONFDIR)/services
RUNLVLDIR   := $(CONFDIR)/runlevels/default

HDRS        := src/common.h src/unit.h
OBJ_COMMON  := src/common.o
OBJ_INIT    := src/init.o src/unit.o

all: ultrainit ult-service ult-journald journalult

ultrainit: $(OBJ_INIT) $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

ult-service: src/ult-service.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

ult-journald: src/journald.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

journalult: src/journalult.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -d -m 0755 $(DESTDIR)$(SBINDIR) $(DESTDIR)$(BINDIR) $(DESTDIR)$(INITDDIR) \
		$(DESTDIR)$(UNITSDIR) $(DESTDIR)$(RUNLVLDIR)
	install -m 0755 ultrainit    $(DESTDIR)$(SBINDIR)/ultrainit
	install -m 0755 ult-service  $(DESTDIR)$(BINDIR)/ult-service
	install -m 0755 ult-journald $(DESTDIR)$(SBINDIR)/ult-journald
	install -m 0755 journalult   $(DESTDIR)$(BINDIR)/journalult
	if [ ! -f $(DESTDIR)$(CONFDIR)/ultrainit.conf ]; then \
		install -m 0644 etc/ultrainit/ultrainit.conf $(DESTDIR)$(CONFDIR)/ultrainit.conf; \
		echo "ultrainit.conf yeni kuruldu."; \
	else \
		echo "ultrainit.conf zaten var, DOKUNULMADI (mevcut ayarlarin korundu)."; \
		echo "Yeni ornek: ultrainit.conf.orig-1.1.0 olarak birakiliyor."; \
		install -m 0644 etc/ultrainit/ultrainit.conf $(DESTDIR)$(CONFDIR)/ultrainit.conf.orig-1.1.0; \
	fi
	install -m 0755 init.d/*      $(DESTDIR)$(INITDDIR)/
	install -m 0644 services/*    $(DESTDIR)$(UNITSDIR)/
	@echo ""
	@echo "Kurulum tamamlandi. Ornek kullanim:"
	@echo "  ult-service list"
	@echo "  ult-service sshd start"
	@echo "  ult-service sshd enable"
	@echo "  ult-service journald enable   # merkezi log (journalult ile oku)"
	@echo "Kernel komut satirina ekleyin: init=$(SBINDIR)/ultrainit"

test: all
	sh tests/run-tests.sh

clean:
	rm -f ultrainit ult-service ult-journald journalult src/*.o

.PHONY: all install test clean
