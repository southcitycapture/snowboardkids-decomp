#!/usr/bin/env python3
"""Run race trials on the G4 and keep the results: the "learning" loop.

Each trial is one headless race (12x real time) driven by the game's own CPU
rider with the given course, character, board, chances and speed edge; the port
prints one result line when player 1 finishes. Results go to a CSV so a sweep
can be resumed and compared.

    nightmare_search.py run course=0,char=3,board=2
    nightmare_search.py sweep 0 1 2      # per-course rider sweep + boost ladder
    nightmare_search.py boost 0          # find the smallest winning boost

Course ids are the game's own: 9 is Rookie Mt. (the one the menu starts on),
0-6 are the bought courses in order. `course=N` aims the character-select
course menu from the port (see port/src/debug/race_dbg.c).
"""
import csv, os, subprocess, sys, time, itertools

G4 = os.path.expanduser("~/Apps/isle-ppc-tools/g4/g4")
SCRIPT = "/Users/zach/race-walk.txt"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nightmare_results.csv")
FIELDS = ["spec", "course", "char", "board", "action", "item", "boost",
          "rank", "finished_before", "frames", "money", "wall_s"]


def g4(*args, **kw):
    return subprocess.run([G4, *args], capture_output=True, text=True, **kw)


def trial(spec, frames=60000, timeout=600):
    g4("stop")
    t0 = time.time()
    g4("run", "--play", SCRIPT, "--headless", "--nightmare",
       "--trial", spec + ",quit=1", "--frames", str(frames))
    log = ""
    while time.time() - t0 < timeout:
        time.sleep(5)
        log = g4("ssh", "grep -E 'sbk-trial: result|EXITCODE' isle-log.txt").stdout
        if "EXITCODE" in log or "sbk-trial: result" in log:
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
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in FIELDS})
    print(row, flush=True)
    return row


def rows():
    if not os.path.exists(OUT):
        return []
    with open(OUT) as f:
        return [r for r in csv.DictReader(f) if r.get("rank")]


def done(spec):
    return any(r["spec"] == spec for r in rows())


def run(spec):
    if done(spec):
        print("skip (already measured):", spec, flush=True)
        return next(r for r in rows() if r["spec"] == spec)
    return save(trial(spec))


def sweep_course(course, riders=((3, 2), (3, 1), (1, 2), (4, 1), (0, 1))):
    """A short rider sweep, then a boost ladder if none of them wins."""
    best = None
    for chr_, board in riders:
        r = run("course=%d,char=%d,board=%d" % (course, chr_, board))
        if not r.get("rank"):
            continue
        key = (int(r["rank"]), int(r["frames"]))
        if best is None or key < best[0]:
            best = (key, chr_, board)
        if int(r["rank"]) == 1:
            break
    if best is None:
        print("course %d: no result" % course, flush=True)
        return
    (rank, frames), chr_, board = best
    print("course %d: best rider char=%d board=%d rank=%d frames=%d" % (course, chr_, board, rank, frames), flush=True)
    if rank == 1:
        return
    for boost in (32, 64, 96, 128):
        r = run("course=%d,char=%d,board=%d,boost=%d" % (course, chr_, board, boost))
        if r.get("rank") == 1:
            print("course %d: wins with boost=%d" % (course, boost), flush=True)
            return
    print("course %d: still losing at boost=128" % course, flush=True)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "run":
        run(sys.argv[2])
    elif len(sys.argv) > 2 and sys.argv[1] == "sweep":
        for c in sys.argv[2:]:
            sweep_course(int(c))
    else:
        print(__doc__)
