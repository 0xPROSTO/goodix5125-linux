#!/usr/bin/env python3
"""End-to-end test of the libfprint driver using libfprint's example programs,
guiding the person at the sensor with desktop notifications.

  drivertest.py BUILD_DIR PSK [--genuine N] [--impostor N] [--skip-enroll]

Phases: enroll (right index slot) -> N verifies with the same finger ->
N verifies with a different finger. Results go to stdout.
"""

import argparse
import os
import re
import select
import subprocess
import sys
import time


def notify(title, body=""):
    subprocess.run(["notify-send", "-a", "Goodix 5125", "-u", "critical",
                    "-t", "8000", title, body], check=False)
    print(f"## {title} {body}", flush=True)


class Prog:

    def __init__(self, build, psk, name):
        env = dict(os.environ,
                   LD_LIBRARY_PATH=os.path.join(build, "libfprint"),
                   GOODIX5125_PSK=psk,
                   G_MESSAGES_DEBUG="libfprint-goodix5125")
        self.p = subprocess.Popen([os.path.join(build, "examples", name)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, env=env)
        self.buf = ""

    def events(self):
        """Yields complete lines, plus pending prompts that lack a newline."""
        fd = self.p.stdout.fileno()
        while True:
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                if self.p.poll() is not None:
                    return
                if self.buf.endswith(("> ", "[Y/n]? ")):
                    line, self.buf = self.buf, ""
                    yield line
                continue
            data = os.read(fd, 4096)
            if not data:
                if self.buf:
                    yield self.buf
                return
            self.buf += data.decode(errors="replace")
            while "\n" in self.buf:
                line, self.buf = self.buf.split("\n", 1)
                yield line

    def send(self, text):
        self.p.stdin.write(text.encode())
        self.p.stdin.flush()


def enroll(build, psk):
    prog = Prog(build, psk, "enroll")
    notify("Регистрация пальца", "Прикладывайте ОДИН палец, естественно, по одному касанию")
    for line in prog.events():
        if "goodix5125-DEBUG" not in line:
            print(line, flush=True)
        if line.endswith("> "):
            prog.send("6\n")
        m = re.search(r"stage (\d+) of (\d+) passed", line)
        if m:
            notify(f"Регистрация: {m.group(1)}/{m.group(2)}", "Уберите и приложите снова")
        elif "failed with error" in line:
            notify("Касание не засчитано", "Тот же палец, так же, как раньше")
    rc = prog.p.wait()
    notify("Регистрация завершена" if rc == 0 else "Регистрация не удалась")
    return rc == 0


def verify(build, psk, count, label, hint):
    prog = Prog(build, psk, "verify")
    results = []
    notify(f"Проверка: {hint}", f"Касание 1/{count}. Угол можно менять")
    for line in prog.events():
        if "goodix5125-DEBUG" in line and "match:" in line:
            print(line, flush=True)
        elif "goodix5125-DEBUG" not in line:
            print(line, flush=True)
        if line.endswith("> "):
            prog.send("6\n")
        if line.startswith("MATCH!") or line.startswith("NO MATCH!"):
            results.append(line.startswith("MATCH!"))
            if len(results) < count:
                notify(f"Проверка: {hint}", f"Касание {len(results) + 1}/{count}")
        if line.endswith("[Y/n]? "):
            prog.send("y\n" if len(results) < count else "n\n")
    prog.p.wait()
    print(f"RESULT {label}: {sum(results)}/{len(results)} matched", flush=True)
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("psk")
    ap.add_argument("--genuine", type=int, default=5)
    ap.add_argument("--impostor", type=int, default=5)
    ap.add_argument("--skip-enroll", action="store_true")
    a = ap.parse_args()
    build, psk = os.path.abspath(a.build), os.path.abspath(a.psk)

    if not a.skip_enroll and not enroll(build, psk):
        sys.exit(1)
    time.sleep(1)
    g = verify(build, psk, a.genuine, "genuine", "ТОТ ЖЕ палец")
    time.sleep(1)
    notify("Теперь ДРУГОЙ палец", "Любой другой, не тот, что регистрировали")
    time.sleep(4)
    i = verify(build, psk, a.impostor, "impostor", "ДРУГОЙ палец")
    notify("Тест окончен", f"свой: {sum(g)}/{len(g)}, чужой: {sum(i)}/{len(i)} совпадений")


if __name__ == "__main__":
    main()
