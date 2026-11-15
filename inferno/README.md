# Inferno on 9vm — build and boot harness

The purgatorio Inferno kernel running natively on the 9vm KVM hypervisor:

- 32-bit x86 kernel, no Linux under it
- root filesystem is **kfs64 on virtio-blk** (`disk/kfs64` serving 9P over a pipe)
- display and input through a shared-memory region the host maps at guest
  physical `0xF8000000` (no 9P in the pixel path)
- Dis JIT enabled at boot (`cflag=1`); a switch back to the interpreter is
  `echo 0 > /dev/jit` inside the guest

This directory holds the scripts that reproduce the build → provision → boot
loop.  Everything here was used to validate the state described at the bottom.

## Files

| file | purpose |
|---|---|
| `mkelf.py` | convert `os/pc/ipc` (Plan 9 a.out) to `ipc.elf` for 9vm, plus `ipc-qemu.elf` for QEMU |
| `aout2elf.py` | the a.out → ELF32 converter mkelf.py calls |
| `provision.sh` | create a 32 MiB image and populate it with `/dis`, `/icons`, `/fonts/misc`, `/lib/wmsetup` |
| `provision.txt` | the shell script fed to the guest (mount `kfs64 -r -b 4096`, copy, mkdir device dirs) |
| `boot.sh` | boot 9vm with `-disk` and optionally `-display-shm` |
| `draindisp.py` | consume committed frames from `/dev/shm/<name>` and write a PPM |
| `sendinput.py` | inject input records (`key 104 0`, `move X Y`, …) into the same region |
| `makesymelf.py` | build `ipc-sym.elf` (symbols) from the a.out, for gdb/symlookup |
| `symlookup.py` | address → kernel symbol, using `ipc-sym.elf` |
| `root.img.gz` | a known-good provisioned 32 MiB kfs64 image (optional; `provision.sh` rebuilds it) |

## Prerequisites

- 9vm built: `make -C .. 9vm`
- a purgatorio checkout next to this one (`../purgatorio`; override with `$PURGATORIO`)
- the Inferno 386 toolchain in `$PURGATORIO/Linux/386/bin` and `mk` on `PATH`
- `$NINEVM_WORK` (default `/tmp/9vm-inferno`) is where build outputs and images go

## Build

```sh
cd ../../purgatorio/os/pc
ROOT=../.. PATH=../../Linux/386/bin:/usr/local/plan9/bin:$PATH mk
cd -
./mkelf.py                # writes $NINEVM_WORK/ipc.elf
```

The kernel embeds `/osinit.dis` and the root tree from `os/pc/pc`, so rebuilding
the init means rebuilding the kernel:

```sh
cd ../../purgatorio/os/init
ROOT=../.. PATH=../../Linux/386/bin:/usr/local/plan9/bin:$PATH mk shell.dis
# then rebuild os/pc again and re-run mkelf.py
```

## Provision a root image

```sh
./provision.sh            # ~1 minute; writes $NINEVM_WORK/root.img
```

or use the bundled baseline:

```sh
mkdir -p /tmp/9vm-inferno
gunzip -c root.img.gz > /tmp/9vm-inferno/root.img
```

`provision.sh` boots a blank disk, runs `disk/kfs64 -r -b 4096` on
`#S/sd00/data`, and copies from the embedded root.  With `-b 4096` the ream
covers the whole disk up front (16 dentries per directory block instead of 4).

## Boot
The serial console is interactive on stdin/stdout.  9vm translates a
terminal Enter (CR) to NL before feeding the guest UART: Purgatorio's
`uartrecv()` puts console bytes into `kbdq` with `kbdputc()` rather than
`kbdcr2nl()`, so a raw terminal would otherwise echo the command but the
shell would never execute it.  `Ctrl-J` (LF) is the workaround on an older
9vm binary.

Headless (serial console on stdin/stdout):

```sh
./boot.sh
```

With the display:

```sh
./boot.sh -s inferno
```

Then, in another shell, grab frames and type at it:

```sh
python3 draindisp.py inferno 10 /tmp/9vm-inferno/frame.ppm   # 10 s of frames
python3 sendinput.py inferno 'key 104 0' 'key 105 0' 'key 13 0'   # h, i, Enter
```

Inside the guest:

```sh
/dis/wm/wm.dis &        # window manager; needs the wm assets on the root
/dis/acme/dis/acme.dis  # editor
```

A healthy `wm` on the kfs root renders a `0x777777` background with the
`0xdddddd` toolbar; `tk: font not found: /fonts/pelm/unicode.8.font` is
expected and harmless (it falls back to the built-in font).

## 9vm host filesystem

The init mounts 9vm's shared-ring host filesystem at `/n/host` when the
hypervisor offers it.  The kernel device is `#~` (`os/pc/devhostring.c`):

- `#~/host` (alias `#~/client`) is a bidirectional 9P2000 byte stream, ready
  for `mount(2)`.
- `#~/ctl` reports the ring address, free buffer slots and request counters.
- 9vm reserves the top 2 MiB of guest RAM and publishes it as a Multiboot
  memory-map entry of type `0x9009`; the device maps it uncached, validates
  the `P9_HOST_MAGIC` header, then submits T-messages and polls completions.
  There is no interrupt from the host, so reads spin briefly and then sleep.
- The exported directory is 9vm's `-r` argument (the current directory by
  default).  The mount is best effort: without the ring the init prints
  `init: no 9ring host transport` and continues.

```sh
mount -A '#~/host' /n/host
ls /n/host
```

## Services

The kernel's init brings up `127.0.0.1`, then starts `/dis/svcd.dis`, the
service manager.  svcd reads `/lib/svcd.conf` or, when present, the host's
`/n/host/svcd.conf`; services are restarted when they exit, with an
exponential backoff:

```
# /n/host/svcd.conf
cpu	/dis/sh.dis -c 'listen -a rc4_128 tcp!*!6668 /dis/sh.dis /lib/cpusession.sh'
```

```
; svcctl
cpu running(group) pid=11 restarts=0
; rcmd -e rc4_128 tcp!127.0.0.1!6668 echo hello
hello
; svcctl stop cpu
; svcctl start cpu
```

`running(group)` means the service's leader exited but its process group is
still alive; `listen` daemonises, so this is normal.  `svcctl log NAME`
shows a service's output and `svcctl ctl` shows svcd's event log.  See
`svcd(8)` and `svcctl(1)` in the purgatorio tree.

`/dis/cpud.dis` is the CLI-only replacement for `auxi/rstyxd`; it mounts the
client's namespace, binds its `/dev`, applies `/lib/cpud.conf` and logs the
session.  `cpu` and `rcmd` are the clients.

### Authentication

Init runs `certify firstboot` before svcd: it creates a signer key in
`/keydb/signerkey` and a certificate for the console user in
`/usr/inferno/keyring/default` if they are missing.  Each machine makes its
own key, so an image cloned **before** first boot carries no shared secret
(one cloned after it carries the original machine's key):

```
init: loopback 127.0.0.1
certify: signer key for signer in /keydb/signerkey
certify: inferno -> /usr/inferno/keyring/default
```

`listen -a rc4_128` authenticates both ends with those certificates and
encrypts the session; `-a none` authenticates but leaves the bytes in the
clear.  The client must name the same algorithm:

```
; rcmd -e rc4_128 tcp!127.0.0.1!6668 echo boot-authed
boot-authed
; rcmd -A tcp!127.0.0.1!6668 echo anon          # no certificate: no session
```

Make a certificate for another user and the session runs as that name:

```
; certify user alice
certify: alice -> /usr/alice/keyring/default
; rcmd -f /usr/alice/keyring/default -e rc4_128 tcp!127.0.0.1!6668 echo hi
hi
; cat /tmp/cpud.log
start user=alice cmd=echo args=echo hi
```

A certificate signed by a different signer is refused
(`pk doesn't match certificate`), and the user name is inside the signed
public key, so a certificate cannot be relabelled.  See `certify(8)`,
`cpud(8)` and `listen(1)`.

What it is not: certificates never expire and cannot be revoked (replacing
one user's certificate leaves the old one valid); `rc4_128` is Inferno's
SSLv2-era RC4/MD5 record layer, good against casual sniffing and no more;
`runas` just writes `/dev/user`, so the user name is accounting, not
containment, and the `cpud` allowlist is the real boundary.

### Modern cryptography

The kernel carries Monocypher 4.0.2 as the builtin module `$Mono`:
XChaCha20-Poly1305, BLAKE2b, X25519 and EdDSA, callable from Dis.  The C
is the Plan 9 port from `factotum-libtab`, adapted for Inferno's `8c`.

```
; monotest
Monocypher 4.0.2
ok   blake2b abc
ok   aead ciphertext not plaintext
ok   aead round trip
ok   aead tampered tag rejected
ok   aead tampered ad rejected
ok   x25519 shared secret
ok   eddsa sign/verify
ok   eddsa changed message rejected
all tests passed
```

`monotest` checks BLAKE2b against the specification's digest of `abc`, an
AEAD round trip with tampered tag and associated data, X25519 agreement, and
EdDSA sign/verify.  `mono(2)` documents the module.

`listen -a mono`, `rcmd -e mono` and `cpu -C mono` select a Monocypher
authentication and transport (`monoauth(8)`): EdDSA certificates from
`certify`, X25519 key exchange, BLAKE2b key derivation and an
XChaCha20-Poly1305 record stream.  One session works end to end, including
one run as a second user:

```
; rcmd -e mono tcp!127.0.0.1!6668 echo mono-ok
mono-ok
```

It is **experimental**: a second session on the same listener hangs, because
mounting and exporting the file-channel descriptor leaks state.  The default
`svcd.conf` still uses `-a rc4_128`; see `man/8/monoauth` in the purgatorio
tree for the protocol and the limits.

## virtio-net

`os/pc/ethervirtio.c` drives 9vm's virtio-net device.  9vm only creates the
PCI function when it is given a tap (`-net-tap`); `boot.sh -n tap0` passes it:

```sh
./boot.sh -s inferno -n tap0
```

Creating a tap normally needs `CAP_NET_ADMIN`, but an unprivileged user+net
namespace is enough, and keeps the tap and the service under test isolated:

```sh
unshare -Urn sh -c '
  ip tuntap add dev tap0 mode tap
  ip addr add 10.0.0.1/24 dev tap0
  ip link set tap0 up
  ip link set lo up
  exec ./boot.sh -n tap0
'
```

The init binds `#l` and `#I` at `/net`.  Configure the guest interface by
hand (or put this in a script in `/n/host`):

```rc
cat /net/ipifc/clone
echo bind ether ether0 > /net/ipifc/0/ctl
echo add 10.0.0.2 255.255.255.0 > /net/ipifc/0/ctl
```

Then any TCP service reachable from the namespace, including a 9P server, can
be mounted directly:

```rc
mkdir /n/cal
mount -A tcp!10.0.0.1!5657 /n/cal
cat /n/cal/calendar.status
```

9vm's ACPI tables do not route the PCI INTx line, so `intrenable` fails for
IRQ 11.  The driver therefore leaves `ether->irq` at -1 and a 10 ms kproc
polls the used rings; the device still raises its ISR bit and the poll reads
it to deassert the line.

## Baseline measured on this setup

- `cp -r /dis` (783 files, 7.6 MB): ~7.7 s
- local syscall on the embedded root: ~40 µs
- 9P RPC on the kfs mount: ~870 µs (8 µs write, ~860 µs waiting for the kfs)
- bare pipe round-trip between two Dis programs: ~40 µs
- filesystem provisioning of the full root: ~1 minute

`^T^T D` on the serial console dumps the Dis scheduler, every `Prog`, and the
kernel stacks; `^T^T K` dumps the console queues and the `kbddrops` counter.
`/dev/jit` reads and writes the Dis JIT flag.

## Known limits

- The kernel is 32-bit (there is no 64-bit `os/` port in Inferno; inferno64 /
  infernode are 64-bit `emu` builds only).  `uvlong` is 4-byte aligned under
  `8c`, so on-disk structures need byte-wise packing.
- kfs64 is a simple write-through 9P server: every block write is synchronous
  and every block allocation rewrites the superblock.  It is frozen at
  cheap fixes; host-side storage is the intended way around it.
- Services are supervised by liveness, not health: a wedged listener is left
  alone.  There is no dependency ordering, per-user service identity, or
  resource limit yet.
- Certificates are bearer tokens with no expiry and no revocation list.
  Adding a user means running `certify user` on the server by hand and
  copying the file to the client out of band.  There is no key directory,
  no per-user allowlist and no audit beyond `/tmp/cpud.log`.
- 9vm has no battery-backed clock, so the guest starts at 1970-01-01.
  Certificates are issued with expiry 0 (never), which is what makes that
  workable; anything that compares dates inside the VM will be wrong.
