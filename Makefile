CLANG    ?= clang
CC       ?= gcc
ARCH     ?= $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')
INCLUDES := -I/usr/include/$(shell uname -m)-linux-gnu

PREFIX     ?= /usr/local
SBIN_DIR   := $(PREFIX)/sbin
LIB_DIR    := $(PREFIX)/lib/xdpdrop
UNIT_DIR   := /etc/systemd/system

BPF_OBJ    := xdp_synrl.o
LOADER     := xdp_synrl_loader
PREP_SH    := xdp-synrl-prep.sh
UNIT       := xdp-synrl.service

CFLAGS_BPF := -O2 -g -Wall -target bpf -D__TARGET_ARCH_$(ARCH) $(INCLUDES)
CFLAGS_USR := -O2 -g -Wall

.PHONY: all clean install uninstall

all: $(BPF_OBJ) $(LOADER)

$(BPF_OBJ): xdp_synrl.c
	$(CLANG) $(CFLAGS_BPF) -c $< -o $@

$(LOADER): xdp_synrl_loader.c
	$(CC) $(CFLAGS_USR) -o $@ $< -lbpf

clean:
	rm -f $(BPF_OBJ) $(LOADER)

install: all
	install -d $(DESTDIR)$(LIB_DIR) $(DESTDIR)$(SBIN_DIR) $(DESTDIR)$(UNIT_DIR)
	install -m 0644 $(BPF_OBJ) $(DESTDIR)$(LIB_DIR)/$(BPF_OBJ)
	install -m 0755 $(LOADER)  $(DESTDIR)$(SBIN_DIR)/$(LOADER)
	install -m 0755 $(PREP_SH) $(DESTDIR)$(SBIN_DIR)/$(PREP_SH)
	install -m 0644 $(UNIT)    $(DESTDIR)$(UNIT_DIR)/$(UNIT)
	systemctl daemon-reload
	systemctl enable $(UNIT)

uninstall:
	systemctl disable --now $(UNIT) || true
	rm -f $(DESTDIR)$(UNIT_DIR)/$(UNIT)
	rm -f $(DESTDIR)$(SBIN_DIR)/$(LOADER) $(DESTDIR)$(SBIN_DIR)/$(PREP_SH)
	rm -f $(DESTDIR)$(LIB_DIR)/$(BPF_OBJ)
	rmdir $(DESTDIR)$(LIB_DIR) 2>/dev/null || true
	systemctl daemon-reload
