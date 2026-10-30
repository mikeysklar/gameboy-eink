"""Write status-gameboy-eink.txt for the tmux status pane, in bright ANSI colors.

Edit TASKS / FINDINGS / NOTE and run: python3 status_pane.py
States: done [x] green, run [~] yellow, todo [ ] white, block [!] red.
"""
import pathlib
import re
import subprocess
import time

OUT = pathlib.Path(__file__).with_name("status-gameboy-eink.txt")  # the file the pane cats
PANE_TITLE = OUT.stem  # "status-TASK", the title the pane was given at split time
ANSI = re.compile(r"\033\[[0-9;]*m")

R = "\033[0m"
B = "\033[1m"
C = {"red": "\033[1;91m", "green": "\033[1;92m", "yellow": "\033[1;93m", "blue": "\033[1;94m",
     "magenta": "\033[1;95m", "cyan": "\033[1;96m", "white": "\033[1;97m", "dim": "\033[2;37m"}
BOX = {"done": (C["green"], "[x]"), "run": (C["yellow"], "[~]"), "todo": (C["white"], "[ ]"),
       "block": (C["red"], "[!]")}

TITLE = "GAME BOY ON E-INK"
SUB = "RP2040 ThinkInk + 4.2in SSD1683, CP 11.0.0-a2 @ 200 MHz"

# (state, text, detail)
TASKS = [
    ("done", "gbeink.mpy: Peanut-GB C, DMG, 2x", ""),
    ("done", "Tetris 51-60 fps on panel", "51.5 avg"),
    ("done", "Mini Gamepad 0x49, 8 inputs, exit", ""),
    ("done", "wipe 30 s, OTP clean 120 s", ""),
    ("done", "clip: tetris-eink-play30-hevc.mp4", "5.6 MB"),
    ("done", "Basecamp: posted to Limor + PT", ""),
    ("done", "reflashed epd-combined (alpha.2-34)", ""),
    ("todo", "code.py is still the PR sign", ""),
]

FINDINGS = [
    "heap 137 KB fresh: buffers before imports",
]

NOTE = "no adafruit/ pushes"


def main():
    lines = [f"{C['cyan']}{TITLE}{R}  {C['dim']}(updated {time.strftime('%H:%M')}){R}",
             f"{C['dim']}{SUB}{R}", ""]
    for state, text, detail in TASKS:
        if state == "head":
            lines.append(f"{C['magenta']}{text}{R}")
            continue
        col, box = BOX[state]
        lines.append(f"  {col}{box}{R} {B}{text}{R}" + (f" {C['dim']}{detail}{R}" if detail else ""))
    lines += [f"{C['blue']}FINDINGS{R}"]
    lines += [f"  {C['white']}- {f}{R}" for f in FINDINGS]
    lines += ["", f"{C['red']}RULES{R} {C['dim']}{NOTE}{R}"]
    OUT.write_text("\n".join(lines) + "\n")
    fit_pane(lines)


def fit_pane(lines):
    # Size the side pane to the longest visible line (colors stripped) plus one
    # column, so nothing wraps and no width is wasted. No pane or no tmux: skip.
    width = max(len(ANSI.sub("", line)) for line in lines) + 1
    try:
        panes = subprocess.run(["tmux", "list-panes", "-a", "-F", "#{pane_id} #{pane_title}"],
                               capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return
    for row in panes.splitlines():
        pane_id, _, title = row.partition(" ")
        if title == PANE_TITLE:
            subprocess.run(["tmux", "resize-pane", "-t", pane_id, "-x", str(width)], check=False)


if __name__ == "__main__":
    main()
