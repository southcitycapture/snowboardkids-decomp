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
          "rank", "finished_before", "frames", "money", "wall_s", "mode"]
# mode: "nopak" for a run measured with no Controller Pak (reproducible, what
# the goldens are made from), "pak" for the older rows measured with one
# plugged in, whose frame counts moved whenever the pak's contents did.


def g4(*args, **kw):
    return subprocess.run([G4, *args], capture_output=True, text=True, **kw)


def trial(spec, frames=60000, timeout=300, extra=()):
    """One headless race. A trial that takes longer than `timeout` is hung
    (a healthy one costs 30-60 s wall at 12x): stop it and return no result,
    so the sweep moves on instead of stalling for ten minutes.

    --nopak and --nopad are what make a trial reproducible. With a Controller
    Pak plugged in the game writes to it during the menus, so the first run
    after any pak change differs from the next ones; and an attached gamepad
    both reports a Rumble Pak (changing the menu prompts) and is claimed only
    when the previous process has let go of it, which is what made a replay of
    the *same* movie land on two different races run to run."""
    g4("stop")
    t0 = time.time()
    g4("run", "--play", SCRIPT, "--headless", "--nightmare", "--nopak", "--nopad",
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
    row = {"spec": spec, "wall_s": round(time.time() - t0), "mode": "nopak"}
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


def update_row(spec, out):
    """Overwrite the measurement of every CSV row with this spec.

    The golden movies are checked against the CSV row they were recorded from,
    so when a row is re-measured (a new build, or the --nopak change that made
    trials reproducible again) the row has to move with it or regress fails on
    a stale number rather than a real regression."""
    if not os.path.exists(OUT) or not out.get("rank"):
        return
    with open(OUT) as f:
        all_rows = list(csv.DictReader(f))
    n = 0
    for r in all_rows:
        if r["spec"] == spec:
            for k in ("rank", "finished_before", "frames", "money", "wall_s", "mode"):
                if k in out:
                    r[k] = out[k]
            n += 1
    with open(OUT, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows([{k: r.get(k, "") for k in FIELDS} for r in all_rows])
    print("csv: %d row(s) for %s updated to rank=%s frames=%s"
          % (n, spec, out.get("rank"), out.get("frames")), flush=True)


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
    # A pak-era row cannot be trusted against a --nopak replay: prefer the
    # reproducible measurements whenever the course has any.
    fresh = [r for r in cands if r.get("mode") == "nopak"]
    if fresh:
        cands = fresh
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
    update_row(spec, out)   # the fresh measurement is the truth, win or lose
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


# ----------------------------------------------------------------- campaign

PAK = "/Users/zach/trial-pak.mpk"
CMDS = "/Users/zach/cmds"


def cmds(*lines):
    """Feed script lines to a running game through --cmds (write, then mv, so
    the game never reads a half-written file)."""
    text = "".join(l + "\n" for l in lines)
    subprocess.run([G4, "ssh", "cat > %s.tmp && mv %s.tmp %s" % (CMDS, CMDS, CMDS)],
                   input=text, text=True)


def nudge(cycles=12):
    """Feed the running game a batch of menu presses through --cmds: on demand,
    so a campaign session can be walked from the results screen into the next
    race without a monkey loose in the shop.

    No START. --soak's monkey presses it, but a START that is still queued when
    the next race starts *pauses* the race, and the campaign then sits on the
    PAUSE / CONTINUE / QUIT / RETRY overlay forever (which is exactly what
    happened on the first Big Snowman run). A alone confirms every prompt the
    campaign meets, and stick-up picks YES."""
    lines = []
    for i in range(cycles):
        lines += ["stick 0 80 3", "wait 6", "press A 3", "wait 45",
                  "press A 3", "wait 45", "press A 3", "wait 45"]
    cmds(*lines)
    print("nudge: %d cycles queued" % cycles, flush=True)


def racing():
    """True while a race is under way (a trial start with no result yet).
    The campaign must not press START during a race: it would pause it."""
    out = g4("ssh", "grep -c 'sbk-trial: start' isle-log.txt; grep -c 'sbk-trial: result' isle-log.txt").stdout.split()
    try:
        return int(out[0]) > int(out[1])
    except (IndexError, ValueError):
        return False


def drive(minutes=60):
    """Campaign autopilot: keep the running session moving through the menus
    between races and report the purse and the win flags as they change."""
    end = time.time() + minutes * 60
    last = None
    while time.time() < end:
        if not racing():
            nudge(2)
        time.sleep(20)
        st = status()
        key = (st.get("money"), st.get("won"), st.get("prog"))
        if key != last:
            last = key
            print("money=%s save=%s prog=%s won=%s unlocks=%s"
                  % (st.get("money"), st.get("savemoney"), st.get("prog"),
                     st.get("won"), st.get("unlocks")), flush=True)
        if st.get("won", "").count("1") >= 8:
            print("every course won", flush=True)
            return


def status():
    """The last sbk-status line of the running game, as a dict."""
    out = g4("ssh", "grep 'sbk-status' isle-log.txt | tail -1").stdout.strip()
    if not out:
        return {}
    d = {}
    for kv in out.split()[1:]:
        if "=" in kv:
            k, v = kv.split("=", 1)
            d[k] = v
    return d


def plan_arg():
    """The rider's book as the port's --plan wants it: COURSE:CHAR:BOARD:BOOST
    for every course that has a winning row in the CSV. With `course=-2` the
    campaign picks its own course, so the port applies the matching row at the
    race's init instead of the trial's fixed char/board."""
    out = []
    for c in COURSES:
        r = best_row(c)
        if r is not None and int(r["rank"]) == 1:
            out.append("%s:%s:%s:%s" % (c, r["char"], r["board"], r["boost"] or 0))
    return ",".join(out)


def campaign_start(spec="course=-2", frames=4000000, save_every=1):
    """A long self-playing session on the experiment pak.

    --autonav drives the menus by name (port/src/debug/menu_nav.c): it parks
    the Game Menu on EXIT / SAVE after every `save_every` races so the purse
    actually reaches the pak, and answers every other screen with A. Every race
    is aimed at the first course still unwon and --status prints the purse and
    the win flags as they change."""
    g4("stop")
    plan = plan_arg()
    g4("run", "--play", SCRIPT, "--turbo", "--nightmare", "--status", "--nopad",
       "--menutrace", "--autonav", "--saveevery", str(save_every),
       "--cmds", CMDS, "--pak", PAK, "--trial", spec, "--plan", plan,
       "--frames", str(frames))
    print("campaign started: spec=%s plan=%s pak=%s" % (spec, plan, PAK), flush=True)


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
    elif len(sys.argv) > 1 and sys.argv[1] == "plan":
        print(plan_arg())
    elif len(sys.argv) > 1 and sys.argv[1] == "campaign":
        campaign_start(*sys.argv[2:])
    elif len(sys.argv) > 1 and sys.argv[1] == "drive":
        drive(int(sys.argv[2]) if len(sys.argv) > 2 else 60)
    elif len(sys.argv) > 1 and sys.argv[1] == "nudge":
        nudge(int(sys.argv[2]) if len(sys.argv) > 2 else 12)
    elif len(sys.argv) > 1 and sys.argv[1] == "status":
        print(status())
    elif len(sys.argv) > 1 and sys.argv[1] == "regress":
        sys.exit(1 if regress([int(c) for c in sys.argv[2:]] or None) else 0)
    else:
        print(__doc__)
