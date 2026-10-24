#!/usr/bin/env python3
"""Collects a labelled capture set for tuning the matcher, prompting the
person at the sensor with desktop notifications.

  dataset.py GXCTL OUT_DIR [touches per finger]

Images are fingerprints: OUT_DIR is created 0700 and must not be shared.
"""

import os
import subprocess
import sys

FINGERS = [
    ("right-index", "правый УКАЗАТЕЛЬНЫЙ"),
    ("right-middle", "правый СРЕДНИЙ"),
    ("right-thumb", "правый БОЛЬШОЙ"),
    ("left-index", "левый УКАЗАТЕЛЬНЫЙ"),
    ("left-thumb", "левый БОЛЬШОЙ"),
]


def notify(title, body=""):
    subprocess.run(["notify-send", "-a", "Goodix 5125", "-u", "critical", "-t", "6000",
                    title, body], check=False)
    print(f"## {title} {body}", flush=True)


def main():
    gxctl, out = sys.argv[1], sys.argv[2]
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    os.makedirs(out, mode=0o700, exist_ok=True)

    for slug, name in FINGERS:
        d = os.path.join(out, slug)
        p = subprocess.Popen([gxctl, "capture", str(per), d], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, bufsize=1,
                             env=dict(os.environ, GX_WAIT_MS="900000"))
        for line in p.stdout:
            print(line, end="", flush=True)
            if "touch the sensor" in line:
                n = line.split("]")[0].strip("[")
                notify(f"{name} палец: {n}",
                       "Приложите естественно; угол можно слегка менять")
        if p.wait() != 0:
            notify("Сбой захвата", slug)
            sys.exit(1)
    notify("Сбор данных окончен", "Спасибо!")


if __name__ == "__main__":
    main()
