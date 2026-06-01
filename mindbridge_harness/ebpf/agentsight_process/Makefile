# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# MindBridge eBPF helper build for the AgentSight-derived process_new tracer.
# This intentionally builds only the process_new monitor, not the full
# AgentSight Rust collector or frontend.

OUTPUT ?= .output
CLANG ?= clang
CC ?= cc
BPFTOOL ?= bpftool
ARCH ?= $(shell uname -m | sed 's/x86_64/x86/' \
	| sed 's/aarch64/arm64/' \
	| sed 's/riscv64/riscv/')
VMLINUX ?= ../vmlinux/$(ARCH)/vmlinux.h

CFLAGS ?= -g -O2 -Wall
LDFLAGS ?=
INCLUDES := -I$(OUTPUT) -I. -I$(dir $(VMLINUX))
LIBS := -lbpf -lelf -lz

CLANG_BPF_SYS_INCLUDES ?= $(shell $(CLANG) -v -E - </dev/null 2>&1 \
	| sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }')

.PHONY: all clean check-deps
all: process_new stdiocap sslsniff

check-deps:
	@command -v $(CLANG) >/dev/null || { echo "missing clang"; exit 1; }
	@command -v $(BPFTOOL) >/dev/null || { echo "missing bpftool"; exit 1; }
	@pkg-config --exists libbpf || { echo "missing libbpf pkg-config metadata"; exit 1; }
	@test -f "$(VMLINUX)" || { echo "missing vmlinux header: $(VMLINUX)"; exit 1; }

$(OUTPUT):
	mkdir -p $(OUTPUT)

$(OUTPUT)/process_new.bpf.o: process_new.bpf.c process_new.h process.h process_ext/*.h $(VMLINUX) | $(OUTPUT)
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
		$(INCLUDES) $(CLANG_BPF_SYS_INCLUDES) \
		-c process_new.bpf.c -o $(OUTPUT)/process_new.tmp.bpf.o
	$(BPFTOOL) gen object $@ $(OUTPUT)/process_new.tmp.bpf.o

$(OUTPUT)/stdiocap.bpf.o: stdiocap.bpf.c stdiocap.h $(VMLINUX) | $(OUTPUT)
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
		$(INCLUDES) $(CLANG_BPF_SYS_INCLUDES) \
		-c stdiocap.bpf.c -o $(OUTPUT)/stdiocap.tmp.bpf.o
	$(BPFTOOL) gen object $@ $(OUTPUT)/stdiocap.tmp.bpf.o

$(OUTPUT)/sslsniff.bpf.o: sslsniff.bpf.c sslsniff.h $(VMLINUX) | $(OUTPUT)
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
		$(INCLUDES) $(CLANG_BPF_SYS_INCLUDES) \
		-c sslsniff.bpf.c -o $(OUTPUT)/sslsniff.tmp.bpf.o
	$(BPFTOOL) gen object $@ $(OUTPUT)/sslsniff.tmp.bpf.o

$(OUTPUT)/process_new.skel.h: $(OUTPUT)/process_new.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)/stdiocap.skel.h: $(OUTPUT)/stdiocap.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)/sslsniff.skel.h: $(OUTPUT)/sslsniff.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)/process_new.o: process_new.c process_new.h process.h process_utils.h process_filter.h container_info.h process_ext/*.h $(OUTPUT)/process_new.skel.h | $(OUTPUT)
	$(CC) $(CFLAGS) $(INCLUDES) $$(pkg-config --cflags libbpf) -c process_new.c -o $@

$(OUTPUT)/stdiocap.o: stdiocap.c stdiocap.h $(OUTPUT)/stdiocap.skel.h | $(OUTPUT)
	$(CC) $(CFLAGS) $(INCLUDES) $$(pkg-config --cflags libbpf) -c stdiocap.c -o $@

$(OUTPUT)/sslsniff.o: sslsniff.c sslsniff.h container_info.h container_utils.h $(OUTPUT)/sslsniff.skel.h | $(OUTPUT)
	$(CC) $(CFLAGS) $(INCLUDES) $$(pkg-config --cflags libbpf) -c sslsniff.c -o $@

process_new: check-deps $(OUTPUT)/process_new.o
	$(CC) $(CFLAGS) $(OUTPUT)/process_new.o $(LDFLAGS) $$(pkg-config --libs libbpf) $(LIBS) -o $@

stdiocap: check-deps $(OUTPUT)/stdiocap.o
	$(CC) $(CFLAGS) $(OUTPUT)/stdiocap.o $(LDFLAGS) $$(pkg-config --libs libbpf) $(LIBS) -o $@

sslsniff: check-deps $(OUTPUT)/sslsniff.o
	$(CC) $(CFLAGS) $(OUTPUT)/sslsniff.o $(LDFLAGS) $$(pkg-config --libs libbpf) $(LIBS) -o $@

clean:
	rm -rf $(OUTPUT) process_new stdiocap sslsniff
