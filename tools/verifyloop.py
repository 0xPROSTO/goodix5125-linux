#!/usr/bin/env python3
"""Runs fprintd-verify repeatedly, prompting with desktop notifications, and
prints each result with the driver's score from the journal (needs
tools/fprintd-debug.sh on).

  verifyloop.py [rounds] [finger label]
"""

import re
import subprocess
import sys
import time


def notify(title, body=""):
    subprocess.run(["notify-send", "-a", "Goodix 5125", "-u", "critical", "-t", "5000",
                    title, body], check=False)


def journal_since(ts):
    out = subprocess.run(["journalctl", "-u", "fprintd", "--no-pager", "-o", "cat",
                          "--since", f"@{ts:.0f}"], capture_output=True, text=True).stdout
    return out.splitlines()


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    label = sys.argv[2] if len(sys.argv) > 2 else "правый УКАЗАТЕЛЬНЫЙ"
    ok = 0
    for i in range(1, rounds + 1):
        ts = time.time() - 1
        notify(f"Проверка {i}/{rounds}", f"Приложи {label} палец, как при разблокировке")
        p = subprocess.run(["fprintd-verify"], capture_output=True, text=True, timeout=120)
        result = re.findall(r"result: (\S+)", p.stdout)
        time.sleep(1)
        lines = journal_since(ts)
        scores = [l for l in lines if "match: print" in l or "capture:" in l]
        matched = "verify-match" in result
        ok += matched
        print(f"{i:2d} {'MATCH' if matched else 'no   '} {result} | {' | '.join(scores)}",
              flush=True)
        time.sleep(1.5)
    notify("Проверка окончена", f"совпало {ok} из {rounds}")
    print(f"matched {ok}/{rounds}")


if __name__ == "__main__":
    main()
