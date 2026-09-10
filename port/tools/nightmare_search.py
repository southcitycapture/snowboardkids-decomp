#!/usr/bin/env python3
"""Run race trials on the G4 and keep the results: the "learning" loop.

Each trial is one headless race (12x real time) driven by the game's CPU
rider with the given character, board, chances and speed edge; the port
prints one result line when player 1 finishes. Results go to a CSV so a
sweep can be resumed and compared.

    nightmare_search.py sweep     # characters x boards, then boosts for the best
    nightmare_search.py run char=2,board=1,boost=16
"""
import csv, os, subprocess, sys, time, itertools

G4 = os.path.expanduser("~/Apps/isle-ppc-tools/g4/g4")
SCRIPT = "/Users/zach/race-walk.txt"
OUT = os.path.expanduser("~/Apps/snowboardkids-decomp/port/tools/nightmare_results.csv")
FIELDS = ["spec", "char", "board", "action", "item", "boost", "rank", "finished_before", "frames", "money", "wall_s"]

def g4(*args, **kw):
    return subprocess.run([G4, *args], capture_output=True, text=True, **kw)

def trial(spec, frames=60000, timeout=900):
    g4("stop")
    t0 = time.time()
    g4("run", "--play", SCRIPT, "--headless", "--nightmare", "--trial", spec + ",quit=1", "--frames", str(frames))
    while time.time() - t0 < timeout:
        time.sleep(8)
        log = g4("ssh", "grep -E 'sbk-trial: result|EXITCODE' isle-log.txt").stdout
        if "EXITCODE" in log:
            break
    row = {"spec": spec, "wall_s": round(time.time() - t0)}
    for line in log.splitlines():
        if line.startswith("sbk-trial: result"):
            for kv in line.split()[2:]:
                k, v = kv.split("=")
                row[k] = int(v)
    return row

def save(row):
    new = not os.path.exists(OUT)
    with open(OUT, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if new: w.writeheader()
        w.writerow({k: row.get(k, "") for k in FIELDS})
    print(row, flush=True)

def sweep():
    for chr_, board in itertools.product(range(5), range(3)):
        save(trial("char=%d,board=%d" % (chr_, board)))
    best = None
    with open(OUT) as f:
        rows = [r for r in csv.DictReader(f) if r["rank"]]
    rows.sort(key=lambda r: (int(r["rank"]), int(r["frames"])))
    best = rows[0]
    print("best rider:", best["spec"], "rank", best["rank"], "frames", best["frames"], flush=True)
    for boost in (8, 16, 32, 64):
        save(trial("%s,boost=%d" % (best["spec"], boost)))

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "run":
        save(trial(sys.argv[2]))
    else:
        sweep()
