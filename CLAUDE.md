@AGENTS.md

## Claude Code notes

- Hardware steps go through the owner: hand them one ready-to-paste command
  (e.g. `./install.sh`) and continue when they say "готово". Never ask for or
  handle the sudo password.
- Once a build is installed, run `tools/cancelrace.py` yourself (no finger
  needed). Use `notify-send` prompts (`tools/latency.py`, `tools/verifyloop.py`)
  when a touch is needed.
- Use the session scratchpad for throwaway experiments. Matcher experiments can
  `#include "lib/match.c"` to reach its static functions. Keep fingerprint
  images out of anything that leaves the machine.
