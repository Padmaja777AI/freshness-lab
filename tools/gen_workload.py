#!/usr/bin/env python3
"""Freshness Lab -- seeded workload generator (HOST SIMULATION inputs).

Writes the workload CSV read by build/flsim (docs/DESIGN.md section 8.4):

    time_ms,action,a,b,c
    <t>,STATE,<stream>,<value>,            (c empty)
    <t>,RAISE,<code>,<deadline_rel>,<retention_rel>
    <t>,CLEAR,<code>,<deadline_rel>,<retention_rel>

Rows are sorted by time_ms; rows with equal time_ms keep generation order
(stable sort), so same-time events get increasing IDs in file order. Every
row satisfies 0 <= time_ms < run_ms.

Determinism: every random quantity comes from one ``random.Random(seed)``
instance created per call; the clock is never read. The draw order is fixed
and documented per scenario (streams in ascending id, then events in the
order listed below), so a given (scenario, seed) always yields identical rows.

Conventions used where the scenario table (docs/DESIGN.md section 10) does
not pin a detail down:
  * Periodic streams use phase offset ``stream * period // n_streams``; for
    healthy_light this is exactly the stated ``stream * 125``. Stream 0 of
    every scenario therefore starts at t = 0 with no offset, which keeps
    alarm_outage stream 0 on the exact 1000 ms grid.
  * A jittered time is clamped into [0, run_ms - 1]; the sample count per
    stream is therefore independent of the seed.
  * Unless a scenario says otherwise, the STATE value is the 0-based sample
    index of that stream (values are opaque 8-byte payloads to the core).

Only the Python standard library is used. This module is importable:
``generate_workload(scenario, seed)`` returns the row tuples,
``SCENARIOS`` maps each scenario name to its flsim config keys (exactly the
keys a scenarios/<name>.cfg file carries), and ``config_lines(scenario)``
renders that cfg file.
"""
import argparse
import csv
import random
import sys

# ---------------------------------------------------------------------------
# Scenario configs: exactly the keys of scenarios/<name>.cfg, in file order.
# ---------------------------------------------------------------------------

CONFIG_KEYS = (
    "run_ms",
    "slot_ms",
    "n_streams",
    "session_id",
    "ack_timeout_ms",
    "max_attempts",
    "aoi_threshold_ms",
    "event_service_ms",
    "slack_guard_ms",
    "state_stale_ms",
    "state_starvation_ms",
)

_COMMON = {
    "run_ms": 30000,
    "slot_ms": 10,
    "n_streams": 4,
    "session_id": 7,
    "ack_timeout_ms": 300,
    "max_attempts": 8,
    "aoi_threshold_ms": 2000,
    "event_service_ms": 40,
    "slack_guard_ms": 100,
    "state_stale_ms": 1000,
    "state_starvation_ms": 2000,
}


def _cfg(**overrides):
    d = dict(_COMMON)
    d.update(overrides)
    return d


SCENARIO_NAMES = (
    "healthy_light",
    "alarm_outage",
    "burst_loss",
    "ack_loss",
    "reorder",
    "overflow",
    "overload",
    "tight_deadline",
)

# Must agree with the committed scenarios/<name>.cfg files (tests/test_tools.py checks it).
# The two outage scenarios use max_attempts=40: 8 attempts x 300 ms exhausted retries inside
# the outage in the seed-101 pilot (docs/DESIGN.md section 10.2); identical for all policies.
SCENARIOS = {
    "healthy_light": _cfg(),
    "alarm_outage": _cfg(n_streams=2, max_attempts=40),
    "burst_loss": _cfg(),
    "ack_loss": _cfg(),
    "reorder": _cfg(),
    "overflow": _cfg(max_attempts=40),
    "overload": _cfg(),
    "tight_deadline": _cfg(n_streams=8),
}

# Seed provenance (docs/DESIGN.md section 10.1): ALL of these are development / pilot
# seeds. The first matrix is exploratory; no held-out evaluation seeds exist yet.
TUNING_SEEDS = (1, 2, 3)
PILOT_SEEDS = (101, 102, 103, 104, 105)
EVALUATION_SEEDS = PILOT_SEEDS  # kept for compatibility; NOT an untouched evaluation set

# Outage schedule shared by the tight_deadline workload and trace generators:
# [start, start + OUTAGE_LEN) for start = OUTAGE_FIRST + k * OUTAGE_PERIOD.
TIGHT_OUTAGE_FIRST_MS = 2000
TIGHT_OUTAGE_PERIOD_MS = 3000
TIGHT_OUTAGE_LEN_MS = 1200


def tight_outage_starts(run_ms):
    """Start times of every tight_deadline outage that begins before run_ms."""
    return list(range(TIGHT_OUTAGE_FIRST_MS, run_ms, TIGHT_OUTAGE_PERIOD_MS))


def in_tight_outage(t):
    """True iff millisecond t lies inside a tight_deadline outage."""
    if t < TIGHT_OUTAGE_FIRST_MS:
        return False
    return (t - TIGHT_OUTAGE_FIRST_MS) % TIGHT_OUTAGE_PERIOD_MS < TIGHT_OUTAGE_LEN_MS


# ---------------------------------------------------------------------------
# Row helpers
# ---------------------------------------------------------------------------

def _state_row(t, stream, value):
    return (t, "STATE", stream, value, "")


def _event_row(t, action, code, deadline_rel, retention_rel):
    return (t, action, code, deadline_rel, retention_rel)


def _clamp(t, run_ms):
    if t < 0:
        return 0
    if t >= run_ms:
        return run_ms - 1
    return t


def _periodic_stream(rng, rows, run_ms, stream, n_streams, period, jitter, value_fn=None):
    """Samples on the grid offset + k*period (k >= 0, grid time < run_ms),
    each shifted by an integer jitter drawn uniformly from [-jitter, jitter]
    (one draw per sample, in k order; no draw when jitter == 0). Times are
    clamped into [0, run_ms - 1]. Value defaults to the sample index k."""
    offset = stream * period // n_streams
    k = 0
    t = offset
    while t < run_ms:
        if jitter > 0:
            tj = _clamp(t + rng.randint(-jitter, jitter), run_ms)
        else:
            tj = t
        value = k if value_fn is None else value_fn(k, t)
        rows.append(_state_row(tj, stream, value))
        k += 1
        t = offset + k * period


def _event_pairs(rng, rows, n_pairs, raise_lo, raise_hi, gap_lo, gap_hi, deadline_rel, retention_rel):
    """n RAISE/CLEAR pairs, codes 1..n. Per pair, in order: RAISE time drawn
    uniformly from [raise_lo, raise_hi], then CLEAR = RAISE + uniform
    [gap_lo, gap_hi]. Pairs are generated in code order."""
    for code in range(1, n_pairs + 1):
        t_raise = rng.randint(raise_lo, raise_hi)
        t_clear = t_raise + rng.randint(gap_lo, gap_hi)
        rows.append(_event_row(t_raise, "RAISE", code, deadline_rel, retention_rel))
        rows.append(_event_row(t_clear, "CLEAR", code, deadline_rel, retention_rel))


# ---------------------------------------------------------------------------
# Scenario workloads
# ---------------------------------------------------------------------------

def _wl_light(rng, cfg, n_pairs):
    """healthy_light / burst_loss / ack_loss: 4 streams every 500 ms, offset
    stream*125, jitter [-50, 50]; n RAISE/CLEAR pairs with RAISE uniform in
    [1000, 27000], CLEAR = RAISE + uniform [200, 1500], deadline 1000,
    retention 10000 (max CLEAR time 28500 < run_ms)."""
    rows = []
    for s in range(cfg["n_streams"]):
        _periodic_stream(rng, rows, cfg["run_ms"], s, cfg["n_streams"], 500, 50)
    _event_pairs(rng, rows, n_pairs, 1000, 27000, 200, 1500, 1000, 10000)
    return rows


def _wl_healthy_light(rng, cfg):
    return _wl_light(rng, cfg, 6)


def _wl_burst_loss(rng, cfg):
    return _wl_light(rng, cfg, 10)


def _wl_ack_loss(rng, cfg):
    return _wl_light(rng, cfg, 6)


def _wl_alarm_outage(rng, cfg):
    """Stream 0 'alarm_active': exactly every 1000 ms, no jitter, value 1 for
    10000 <= t < 12000 else 0. The explicit rows (1 at 10000, 0 at 12000)
    coincide with grid rows and are emitted once. Stream 1 'temperature':
    every 500 ms (offset 250), jitter [-50, 50], value = random walk from 200
    with steps uniform in [-3, 3] (one step per sample, drawn after that
    sample's jitter). Events: RAISE code 1 at 10000, CLEAR code 1 at 12000,
    deadline 1000, retention 17000. Only stream 1 consumes randomness."""
    rows = []
    run_ms = cfg["run_ms"]
    alarm_on_lo, alarm_on_hi = 10000, 12000

    def alarm_value(_k, t):
        return 1 if alarm_on_lo <= t < alarm_on_hi else 0

    _periodic_stream(rng, rows, run_ms, 0, cfg["n_streams"], 1000, 0, alarm_value)
    grid_times = set(r[0] for r in rows)
    for t_explicit, v_explicit in ((alarm_on_lo, 1), (alarm_on_hi, 0)):
        if t_explicit not in grid_times and t_explicit < run_ms:
            rows.append(_state_row(t_explicit, 0, v_explicit))

    walk = {"v": 200}

    def temperature(_k, _t):
        walk["v"] += rng.randint(-3, 3)
        return walk["v"]

    _periodic_stream(rng, rows, run_ms, 1, cfg["n_streams"], 500, 50, temperature)
    rows.append(_event_row(alarm_on_lo, "RAISE", 1, 1000, 17000))
    rows.append(_event_row(alarm_on_hi, "CLEAR", 1, 1000, 17000))
    return rows


def _wl_reorder(rng, cfg):
    """4 streams every 200 ms (offset stream*50), jitter [-20, 20]; 6 pairs
    as in healthy_light (deadline 1000, retention 10000)."""
    rows = []
    for s in range(cfg["n_streams"]):
        _periodic_stream(rng, rows, cfg["run_ms"], s, cfg["n_streams"], 200, 20)
    _event_pairs(rng, rows, 6, 1000, 27000, 200, 1500, 1000, 10000)
    return rows


def _wl_overflow(rng, cfg):
    """4 streams every 500 ms (offset stream*125), jitter [-50, 50].
    20 RAISE at 5100 + 100*i (i = 0..19), codes 1..20, then 20 CLEAR at
    13000 + 100*i, codes 1..20; deadline 2000, retention 8000. With
    FL_EVENT_CAPACITY = 8 and the 5000..12000 outage, RAISEs 9..20 (12 of
    them) must be REJECTED_FULL: no slot can free before the last RAISE at
    7000 because retention (13100+) and retry exhaustion (first tx >= 5100
    plus 7 timeouts of 300 ms >= 7200, exhausted at >= 7500) both come later.
    Events consume no randomness."""
    rows = []
    for s in range(cfg["n_streams"]):
        _periodic_stream(rng, rows, cfg["run_ms"], s, cfg["n_streams"], 500, 50)
    for i in range(20):
        rows.append(_event_row(5100 + 100 * i, "RAISE", i + 1, 2000, 8000))
    for i in range(20):
        rows.append(_event_row(13000 + 100 * i, "CLEAR", i + 1, 2000, 8000))
    return rows


def _wl_overload(rng, cfg):
    """4 streams every 250 ms (offset stream*62), jitter [-20, 20].
    RAISE every 8 ms from 5000 to 24992 inclusive: (24992-5000)/8 + 1 = 2500
    events, code (i % 250) + 1, deadline 200, retention 1000. Events consume
    no randomness."""
    rows = []
    for s in range(cfg["n_streams"]):
        _periodic_stream(rng, rows, cfg["run_ms"], s, cfg["n_streams"], 250, 20)
    for i in range(2500):
        rows.append(_event_row(5000 + 8 * i, "RAISE", (i % 250) + 1, 200, 1000))
    return rows


def _wl_tight_deadline(rng, cfg):
    """8 streams every 100 ms (offset stream*12), jitter [-10, 10].
    Outages [2000+3000k, 3200+3000k). For each outage ending at E with
    E + 10 < run_ms: RAISE code 1 at E+10 and CLEAR code 1 at E+500
    (deadline 400, retention 3000); with run_ms = 30000 that is E = 3200,
    6200, ..., 27200 (9 pairs; the outage starting at 29000 ends after the
    run). Then 20 extra RAISE code 2 (deadline 400, retention 3000) at times
    drawn uniformly from [1000, 29000] by rejection sampling until the time
    is outside every outage (redraws consume randomness)."""
    rows = []
    run_ms = cfg["run_ms"]
    for s in range(cfg["n_streams"]):
        _periodic_stream(rng, rows, run_ms, s, cfg["n_streams"], 100, 10)
    for start in tight_outage_starts(run_ms):
        end = start + TIGHT_OUTAGE_LEN_MS
        if end + 10 >= run_ms:
            continue
        rows.append(_event_row(end + 10, "RAISE", 1, 400, 3000))
        if end + 500 < run_ms:
            rows.append(_event_row(end + 500, "CLEAR", 1, 400, 3000))
    for _ in range(20):
        t = rng.randint(1000, 29000)
        while in_tight_outage(t):
            t = rng.randint(1000, 29000)
        rows.append(_event_row(t, "RAISE", 2, 400, 3000))
    return rows


_GENERATORS = {
    "healthy_light": _wl_healthy_light,
    "alarm_outage": _wl_alarm_outage,
    "burst_loss": _wl_burst_loss,
    "ack_loss": _wl_ack_loss,
    "reorder": _wl_reorder,
    "overflow": _wl_overflow,
    "overload": _wl_overload,
    "tight_deadline": _wl_tight_deadline,
}


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def generate_workload(scenario, seed):
    """Return the workload rows for (scenario, seed) as a list of 5-tuples
    (time_ms, action, a, b, c), sorted by time_ms with ties in generation
    order. Raises ValueError for an unknown scenario."""
    if scenario not in SCENARIOS:
        raise ValueError("unknown scenario: %s" % scenario)
    cfg = SCENARIOS[scenario]
    rng = random.Random(seed)
    rows = _GENERATORS[scenario](rng, cfg)
    rows.sort(key=lambda r: r[0])  # stable: same-time rows keep generation order
    run_ms = cfg["run_ms"]
    n_streams = cfg["n_streams"]
    for r in rows:
        if not 0 <= r[0] < run_ms:
            raise AssertionError("row time %d outside [0, %d)" % (r[0], run_ms))
        if r[1] == "STATE":
            if not 0 <= r[2] < n_streams:
                raise AssertionError("stream %d outside n_streams=%d" % (r[2], n_streams))
        elif r[1] in ("RAISE", "CLEAR"):
            if not (1 <= r[2] <= 255 and 0 <= r[3] <= r[4]):
                raise AssertionError("bad event row %r" % (r,))
        else:
            raise AssertionError("bad action %r" % (r,))
    return rows


def count_events(rows):
    """Number of RAISE/CLEAR rows."""
    return sum(1 for r in rows if r[1] in ("RAISE", "CLEAR"))


def config_lines(scenario):
    """The scenarios/<name>.cfg contents (key=value lines) for a scenario."""
    if scenario not in SCENARIOS:
        raise ValueError("unknown scenario: %s" % scenario)
    cfg = SCENARIOS[scenario]
    return ["%s=%d" % (k, cfg[k]) for k in CONFIG_KEYS]


def write_workload_csv(rows, fp):
    w = csv.writer(fp, lineterminator="\n")
    w.writerow(["time_ms", "action", "a", "b", "c"])
    for r in rows:
        w.writerow(list(r))


def main(argv=None):
    p = argparse.ArgumentParser(description="Freshness Lab seeded workload generator (HOST SIMULATION inputs)")
    p.add_argument("--scenario", help="scenario name (see --list)")
    p.add_argument("--seed", type=int, help="integer seed for random.Random")
    p.add_argument("--out", help="output workload CSV path ('-' for stdout)")
    p.add_argument("--cfg-out", help="also write the scenario's config file (key=value lines) here")
    p.add_argument("--list", action="store_true", help="print scenario names and exit")
    args = p.parse_args(argv)
    if args.list:
        for name in SCENARIO_NAMES:
            print(name)
        return 0
    if args.scenario is None or args.seed is None or (args.out is None and args.cfg_out is None):
        p.error("--scenario, --seed and --out are required (or --list)")
    if args.scenario not in SCENARIOS:
        p.error("unknown scenario %r; use --list" % args.scenario)
    rows = generate_workload(args.scenario, args.seed)
    if args.out is not None:
        if args.out == "-":
            write_workload_csv(rows, sys.stdout)
        else:
            with open(args.out, "w", newline="") as fp:
                write_workload_csv(rows, fp)
    if args.cfg_out is not None:
        with open(args.cfg_out, "w") as fp:
            fp.write("\n".join(config_lines(args.scenario)) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
