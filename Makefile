CC = clang
CFLAGS = -Wall -Wextra -O2
# The sanitizer runtimes come with gcc here, not with this clang.
TESTCC = gcc
PAYLOAD_CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie

all: 9vm test_payload.elf

9vm: 9vm.c hostfs.c hostfs.h vmdisplay.h
	$(CC) $(CFLAGS) -pthread -o 9vm 9vm.c hostfs.c

test_payload.elf: test_payload.S
	$(CC) $(PAYLOAD_CFLAGS) -Wl,-Ttext,0x100000 -o test_payload.elf test_payload.S

# hostfs against a hostile guest, under ASan and UBSan.  hostfs reads 9P
# fields through unaligned pointers, which x86-64 and ARM64 allow.
test_hostfs: test_hostfs.c hostfs.c hostfs.h vmdisplay.h
	$(TESTCC) $(CFLAGS) -g -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all -o $@ test_hostfs.c hostfs.c

check: test_hostfs
	./test_hostfs

kvm-arm64-smoke: kvm-arm64-smoke.c
	$(CC) $(CFLAGS) -o $@ $<

# Host-side board layer; builds and runs only on an ARM64 host with KVM
# (the ARM ioctls are absent from x86 headers), so it is not in `all`.
9vm-arm64: 9vm-arm64.c hostfs.c hostfs.h vmdisplay.h
	$(CC) $(CFLAGS) -pthread -o $@ 9vm-arm64.c hostfs.c

kvm-arm64-probe: kvm-arm64-probe.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f 9vm test_payload.elf test_hostfs kvm-arm64-smoke 9vm-arm64 kvm-arm64-probe

.PHONY: all check clean
