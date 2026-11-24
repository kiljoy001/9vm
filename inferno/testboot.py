#!/usr/bin/env python3
"""Boot a freshly provisioned image N times and fail on a crash.

    ./testboot.py [-n runs] [-m mb] [--jit] [--timeout s] [image]

Every run boots its own copy of the image, so each starts from the same
state: the first boot after provisioning (nothing in /keydb, a clean kfs
image).  This is the boot that has been failing: the guest panics,
reports a lock loop, or never reaches a shell prompt.

With --jit, /n/host/jit is created for the run, which is how the Dis JIT
is turned on (see the JIT note in README.md).  The JIT is off by
default, so a plain run tests the interpreter.

Exit status is 0 only if every boot reached the shell.
"""
import argparse
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def boot(elf, image, memory, timeout):
    """Boot one copy of image; return (verdict, console text)."""
    proc = subprocess.Popen(
        [str(REPO / "9vm"), "-m", str(memory),
         "-c", "bootargs=local!#9/0 nobootprompt=local console=0",
         "-disk", str(image), str(elf)],
        cwd=HERE, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    chunks = queue.Queue()

    def read():
        fd = proc.stdout.fileno()
        while True:
            data = os.read(fd, 4096)
            if not data:
                chunks.put(None)
                return
            chunks.put(data.decode("utf-8", "replace"))

    threading.Thread(target=read, daemon=True).start()

    text = ""
    deadline = time.monotonic() + timeout
    verdict = "timeout"
    while time.monotonic() < deadline:
        try:
            chunk = chunks.get(timeout=0.3)
        except queue.Empty:
            continue
        if chunk is None:
            break
        text += chunk
        if "panic:" in text or "lock loop" in text:
            verdict = "crash"
            time.sleep(3)                  # let the panic print in full
            try:
                while True:
                    text += chunks.get_nowait() or ""
            except queue.Empty:
                pass
            break
        if "dns forwarding" in text and "; " in text:
            verdict = "ok"
            break
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    return verdict, text


def main():
    work = Path(os.environ.get("NINEVM_WORK", "/tmp/9vm-inferno"))
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--runs", type=int, default=8)
    ap.add_argument("-m", "--memory", type=int, default=1024,
                    help="guest RAM in MB (default 1024)")
    ap.add_argument("--jit", action="store_true",
                    help="enable the Dis JIT for these boots (/n/host/jit)")
    ap.add_argument("--timeout", type=int, default=120,
                    help="seconds per boot (default 120)")
    ap.add_argument("image", type=Path, nargs="?", default=work / "root.img")
    args = ap.parse_args()

    elf = work / "ipc.elf"
    for path in (args.image, elf):
        if not path.exists():
            sys.exit(f"missing {path} (run ./mkelf.py and ./provision.sh)")

    jit = HERE / "jit"
    if args.jit:
        jit.write_text("1\n")
    failures = []
    copy = work / "testboot.img"
    try:
        for i in range(args.runs):
            shutil.copyfile(args.image, copy)
            verdict, text = boot(elf, copy, args.memory, args.timeout)
            print(f"{i}: {verdict}", flush=True)
            if verdict != "ok":
                failures.append((i, verdict, text))
                break
    finally:
        if args.jit:
            jit.unlink(missing_ok=True)
        copy.unlink(missing_ok=True)

    if failures:
        i, verdict, text = failures[0]
        print(f"FAIL: boot {i}: {verdict}", file=sys.stderr)
        print("\n".join(text.splitlines()[-40:]), file=sys.stderr)
        return 1
    print(f"{args.runs} boots reached the shell")
    return 0


if __name__ == "__main__":
    sys.exit(main())
