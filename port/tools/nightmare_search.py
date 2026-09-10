#!/usr/bin/env python3
"""Run race trials on the G4 and keep the results: the "learning" loop.

Each trial is one headless race (12x real time) driven by the game's own CPU
rider with the given course, character, board, chances and speed edge; the port
prints one result line when player 1 finishes. Results go to a CSV so a sweep
can be resumed and compared.

    nightmare_search.py run course=0,char=3,board=2
    nightmare_search.py sweep 0 1 2      # per-course rider sweep + boost ladder
    nightmare_search.py boost 0          # find the smallest winning boost
    nightmare_search.py table            # best setup per course, from the CSV
    nightmare_search.py record 0 1       # re-run the winners with --record
    nightmare_search.py regress          # replay every golden movie, pass/fail

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


def trial(spec, frames=60000, timeout=300, extra=()):
    """One headless race. A trial that takes longer than `timeout` is hung
    (a healthy one costs 30-60 s wall at 12x): stop it and return no result,
    so the sweep moves on instead of stalling for ten minutes."""
    g4("stop")
    t0 = time.time()
    g4("run", "--play", SCRIPT, "--headless", "--nightmare",
       "--trial", spec + ",quit=1", "--frames", str(frames), *extra)
    log = ""
    while time.time() - t0 < timeout:
        time.sleep(5)
        log = g4("ssh", "grep -E 'sbk-trial: result|EXITCODE' isle-log.txt").stdout
        if "EXITCODE" in log or "sbk-trial: result" in log:
            break
    else:
        print("hung trial (>%ds), stopping: %s" % (timeout, spec), flush=True)
        g4("stop")
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



# ---------------------------------------------------------------- reporting

def best_row(course):
    """The best measured setup for a course: lowest (rank, frames)."""
    cands = [r for r in rows() if r["course"] == str(course)]
    if not cands:
        return None
    return min(cands, key=lambda r: (int(r["rank"]), int(r["frames"])))


def table():
    for c in COURSES:
        r = best_row(c)
        if r is None:
            print("course %-2s  no result" % c)
        else:
            print("course %-2s  char=%s board=%s boost=%-3s rank=%s frames=%-6s  %s"
                  % (c, r["char"], r["board"], r["boost"], r["rank"], r["frames"],
                     COURSE_NAMES.get(int(c), "")))


# ------------------------------------------------------------ golden movies

GOLDEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts", "golden")


def golden_spec(course):
    """The winning spec for a course, as recorded in the CSV."""
    r = best_row(course)
    if r is None or int(r["rank"]) != 1:
        return None, r
    spec = "course=%s,char=%s,board=%s" % (r["course"], r["char"], r["board"])
    if int(r["boost"] or 0):
        spec += ",boost=%s" % r["boost"]
    return spec, r


def record(course):
    """Replay the winning setup once more with --record, and keep the movie.

    The rider is the game's own CPU logic, so the movie holds the *script's*
    controller input (the menu walk); replaying it needs the same trial spec,
    which regress() reads back out of the CSV.
    """
    spec, r = golden_spec(course)
    if spec is None:
        print("course %s: no winning row to record" % course, flush=True)
        return None
    remote = "/Users/zach/golden-course%s.m64" % course
    out = trial(spec, extra=("--record", remote))
    out["spec"] = spec
    if out.get("rank") != 1:
        print("course %s: record run did not win (%r), movie not kept" % (course, out), flush=True)
        return None
    os.makedirs(GOLDEN, exist_ok=True)
    local = os.path.join(GOLDEN, "course%s.m64" % course)
    g4("pull", remote, local)
    print("course %s: golden movie %s (rank=1 frames=%s)" % (course, local, out["frames"]), flush=True)
    return local


def regress(courses=None):
    """Replay every golden movie headless and check it still wins the same race.

    Each movie is replayed with the trial spec its CSV row was measured with;
    a run passes when the rank matches and the frame count is identical
    (the port is deterministic, so any drift is a real regression).
    """
    if courses is None:
        courses = [c for c in COURSES if os.path.exists(os.path.join(GOLDEN, "course%s.m64" % c))]
    fails = 0
    print("%-8s %-28s %-14s %-14s %s" % ("course", "spec", "expected", "got", "result"))
    for c in courses:
        local = os.path.join(GOLDEN, "course%s.m64" % c)
        spec, r = golden_spec(c)
        if spec is None or not os.path.exists(local):
            print("%-8s %-28s %-14s %-14s %s" % (c, spec or "-", "-", "-", "SKIP (no movie)"))
            continue
        remote = "/Users/zach/regress-course%s.m64" % c
        subprocess.run([G4, "ssh", "cat > %s" % remote], stdin=open(local, "rb"))
        got = trial(spec, extra=("--play", remote))
        exp = "rank=%s/%s" % (r["rank"], r["frames"])
        gots = "rank=%s/%s" % (got.get("rank"), got.get("frames"))
        ok = got.get("rank") == int(r["rank"]) and got.get("frames") == int(r["frames"])
        fails += not ok
        print("%-8s %-28s %-14s %-14s %s" % (c, spec, exp, gots, "PASS" if ok else "FAIL"), flush=True)
    print("%d course(s) checked, %d failed" % (len(courses), fails))
    return fails


COURSES = [9, 0, 1, 2, 3, 4, 5, 6]
# Course ids in the game's own order (from the asset table in include/assets.h).
COURSE_NAMES = {0: "Big Snowman", 1: "Sunset Rock", 2: "Night Highway", 3: "Grass Valley",
                4: "Dizzy Land", 5: "Quicksand Valley", 6: "Silver Mountain",
                7: "Animal Land", 8: "Ninja Land", 9: "Rookie Mountain"}

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "run":
        run(sys.argv[2])
    elif len(sys.argv) > 2 and sys.argv[1] == "sweep":
        for c in sys.argv[2:]:
            sweep_course(int(c))
    elif len(sys.argv) > 1 and sys.argv[1] == "table":
        table()
    elif len(sys.argv) > 1 and sys.argv[1] == "record":
        for c in (sys.argv[2:] or COURSES):
            record(int(c))
    elif len(sys.argv) > 1 and sys.argv[1] == "regress":
        sys.exit(1 if regress([int(c) for c in sys.argv[2:]] or None) else 0)
    else:
        print(__doc__)
