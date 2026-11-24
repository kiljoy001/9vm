#!/usr/bin/env python3
"""Boot the Inferno guest and check its clock against the host's.

    ./testclock.py [--image root.img] [--elf ipc.elf] [--boot-script boot.sh]

9vm serves the host clock as the virtual file /n/host/time, read at the
moment of the request.  init(8) sets /dev/time from it at boot and
timed(8) holds the guest to it.  This test boots the guest on the serial
console and checks, from inside:

  1. /n/host/time is live (two readings five seconds apart differ by five
     seconds, not zero),
  2. the guest clock is within a few hundred milliseconds of it,
  3. a corrupted guest clock (set to 1970) is stepped back to the host's.

It needs a provisioned root image (./provision.sh) and the converted
kernel (./mkelf.py).  Exit status is 0 on success, 1 on failure.
"""
import argparse
import os
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOLERANCE_US = 400_000          # wall clock vs host, after the first reading
SETTLED_US = 200_000            # ... once the service has had a few seconds


class Console:
    """The 9vm serial console on a pipe, with a marker-driven reader."""

    def __init__(self, argv, cwd, env=None):
        self.proc = subprocess.Popen(
            argv, cwd=cwd, env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.chunks = queue.Queue()
        self.text = ""
        self.seq = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        fd = self.proc.stdout.fileno()
        while True:
            data = os.read(fd, 4096)
            if not data:
                self.chunks.put(None)
                return
            self.chunks.put(data.decode("utf-8", "replace"))

    def drain(self, timeout=0.2):
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return
            try:
                chunk = self.chunks.get(timeout=left)
            except queue.Empty:
                return
            if chunk is None:
                return
            self.text += chunk

    def wait_for(self, marker, timeout):
        return self.wait_for_re(re.escape(marker), timeout)

    def wait_for_re(self, pattern, timeout):
        regex = re.compile(pattern)
        end = time.monotonic() + timeout
        while not regex.search(self.text):
            if time.monotonic() > end:
                raise TimeoutError(f"no /{pattern}/ after {timeout}s")
            if self.proc.poll() is not None:
                self.drain(0.5)
                raise TimeoutError(f"9vm exited before /{pattern}/")
            try:
                chunk = self.chunks.get(timeout=0.5)
            except queue.Empty:
                continue
            if chunk is None:
                raise TimeoutError(f"9vm closed the console before /{pattern}/")
            self.text += chunk
        return self.text

    def send(self, line):
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def probe(console, wait=0):
    """Return (host_us, guest_us) as the guest reads them."""
    if wait:
        time.sleep(wait)
    tag = f"probe{console.seq}b"
    end = f"probe{console.seq}e"
    console.seq += 1
    console.text = ""
    console.send(f"echo {tag}; cat /n/host/time; cat /dev/time; echo; echo {end}")
    console.wait_for_re(r"(?m)^" + end + r"\s*$", 30)
    values = [int(m.group(1)) for m in
              re.finditer(r"(?m)^(\d{10,})", console.text)]
    if len(values) < 2:
        raise AssertionError(
            f"expected host and guest times, got {values!r} from:\n{console.text}")
    return values[0], values[1]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    work = Path(os.environ.get("NINEVM_WORK", "/tmp/9vm-inferno"))
    ap.add_argument("--image", type=Path, default=work / "root.img")
    ap.add_argument("--elf", type=Path, default=work / "ipc.elf")
    ap.add_argument("--boot-script", type=Path, default=HERE / "boot.sh")
    args = ap.parse_args()

    for path in (args.image, args.elf, args.boot_script):
        if not path.exists():
            sys.exit(f"missing {path} (run ./mkelf.py and ./provision.sh)")

    failures = []
    console = None
    try:
        console = Console(["sh", str(args.boot_script),
                           str(args.image), str(args.elf)], cwd=HERE)
        console.wait_for("dns forwarding", 120)
        console.drain(1.0)

        host1, guest1 = probe(console)
        host2, guest2 = probe(console, wait=5)
        elapsed = host2 - host1
        if not 4_500_000 <= elapsed <= 5_500_000:
            failures.append(
                f"/n/host/time is stale: {elapsed} us between readings 5s apart")
        off1 = guest1 - host1
        off2 = guest2 - host2
        if abs(off1) > TOLERANCE_US:
            failures.append(f"guest clock off by {off1} us at first reading")
        if abs(off2) > SETTLED_US:
            failures.append(f"guest clock off by {off2} us after 5s")
        print(f"host clock live: {elapsed/1e6:.3f}s per 5s; "
              f"offset {off1/1000:.1f}ms then {off2/1000:.1f}ms")

        # A wrong guest clock must be stepped back to the host's time.
        console.text = ""
        console.send("echo 1000000 > /dev/time")
        time.sleep(3)
        host3, guest3 = probe(console)
        off3 = guest3 - host3
        if abs(off3) > SETTLED_US:
            failures.append(
                f"guest clock not recovered after being set to 1970: {off3} us")
        print(f"recovery after 1970: offset {off3/1000:.1f}ms")

        # The host's time file is read only.  A failed redirection ends the
        # shell's line, so the marker goes on a line of its own.
        console.text = ""
        console.send("echo x > /n/host/time")
        time.sleep(1.0)
        console.send("echo write1")
        console.wait_for_re(r"(?m)^write1\s*$", 20)
        if "time is read only" not in console.text:
            failures.append("a write to /n/host/time was not refused")
        console.text = ""
        console.send("rm /n/host/time")
        time.sleep(1.0)
        console.send("echo remove1")
        console.wait_for_re(r"(?m)^remove1\s*$", 20)
        if "time cannot be removed" not in console.text:
            failures.append("a remove of /n/host/time was not refused")
    except (TimeoutError, AssertionError, OSError) as e:
        failures.append(str(e))
    finally:
        if console is not None:
            console.stop()

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("clock test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
