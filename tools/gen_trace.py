#!/usr/bin/env python3
"""Freshness Lab -- seeded matched channel trace generator (HOST SIMULATION).

Writes the trace CSV read by build/flsim (docs/DESIGN.md section 8.2):

    slot,data_lost,data_delay_ms,ack_lost,ack_delay_ms

with exactly run_ms // slot_ms consecutive rows (slots 0..n-1), losses in
{0, 1} and delays >= 1 ms. Slot k covers the transmit opportunity at
t = k * slot_ms in both directions; every policy reads the same entry for
the same slot, which is what makes runs matched.

Determinism: every random quantity comes from one ``random.Random(seed)``
instance per call; the clock is never read. Per slot the draw order is
fixed: data loss, (burst_loss only) channel-state transition, ack loss,
then any delay draws (data delay before ack delay). Slots that a scenario
forces (outages) consume no randomness.

Only the Python standard library is used. Importable API:
``generate_trace(scenario, seed)`` returns the row tuples and ``SCENARIOS``
(shared with gen_workload.py) maps scenario names to their config keys.
"""
import argparse
import csv
import os
import random
import sys


def _load_workload_module():
    """gen_workload.py from this directory, whether this file is run as a
    script, imported with tools/ on sys.path, or loaded by file path."""
    try:
        import gen_workload  # tools/ on sys.path (script or sys.path insert)
        return gen_workload
    except ImportError:
        pass
    import importlib.util
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen_workload.py")
    spec = importlib.util.spec_from_file_location("gen_workload", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_wl = _load_workload_module()
SCENARIOS = _wl.SCENARIOS
SCENARIO_NAMES = _wl.SCENARIO_NAMES
in_tight_outage = _wl.in_tight_outage

DEFAULT_DELAY_MS = 20


def _iid(rng, p):
    """One Bernoulli(p) draw as 0/1; p == 0 or p == 1 still consumes a draw
    so that the per-slot draw order is identical for every scenario."""
    return 1 if rng.random() < p else 0


# ---------------------------------------------------------------------------
# Scenario traces: each returns (data_lost, data_delay, ack_lost, ack_delay)
# for slot k at time t, possibly consuming randomness.
# ---------------------------------------------------------------------------

def _tr_iid(p_data, p_ack):
    def f(rng, _k, _t, _st):
        dl = _iid(rng, p_data)
        al = _iid(rng, p_ack)
        return dl, DEFAULT_DELAY_MS, al, DEFAULT_DELAY_MS
    return f


def _tr_alarm_outage(_rng, _k, t, _st):
    """Loss 0 everywhere except 9000 <= t < 15000: loss 1 both directions.
    No randomness at all (the seed only affects the workload's stream 1)."""
    lost = 1 if 9000 <= t < 15000 else 0
    return lost, DEFAULT_DELAY_MS, lost, DEFAULT_DELAY_MS


def _tr_burst_loss(rng, _k, _t, st):
    """Data direction: Gilbert-Elliott, state per slot, starting good.
    Draw order per slot: data loss with the current state's loss probability
    (good 0.01, bad 0.9), then the transition draw (good->bad 0.02,
    bad->good 0.1) which sets the state for the next slot, then the iid
    ack loss (0.01). Delay 20 both."""
    good = st.get("good", True)
    dl = _iid(rng, 0.01 if good else 0.9)
    if good:
        if rng.random() < 0.02:
            good = False
    else:
        if rng.random() < 0.1:
            good = True
    st["good"] = good
    al = _iid(rng, 0.01)
    return dl, DEFAULT_DELAY_MS, al, DEFAULT_DELAY_MS


def _tr_reorder(rng, _k, _t, _st):
    """Loss iid 0.01 both; data_delay = 200 with p = 0.3 else 20, then
    independently ack_delay = 200 with p = 0.3 else 20."""
    dl = _iid(rng, 0.01)
    al = _iid(rng, 0.01)
    dd = 200 if rng.random() < 0.3 else DEFAULT_DELAY_MS
    ad = 200 if rng.random() < 0.3 else DEFAULT_DELAY_MS
    return dl, dd, al, ad


def _tr_overflow(rng, _k, t, _st):
    """Loss 1 both directions for 5000 <= t < 12000 (no draws), else iid
    0.01 both. Delay 20."""
    if 5000 <= t < 12000:
        return 1, DEFAULT_DELAY_MS, 1, DEFAULT_DELAY_MS
    dl = _iid(rng, 0.01)
    al = _iid(rng, 0.01)
    return dl, DEFAULT_DELAY_MS, al, DEFAULT_DELAY_MS


def _tr_tight_deadline(rng, _k, t, _st):
    """Outages [2000+3000k, 3200+3000k): loss 1 both directions (no draws).
    Outside outages: data loss iid 0.20, ack loss iid 0.05. Delay 20."""
    if in_tight_outage(t):
        return 1, DEFAULT_DELAY_MS, 1, DEFAULT_DELAY_MS
    dl = _iid(rng, 0.20)
    al = _iid(rng, 0.05)
    return dl, DEFAULT_DELAY_MS, al, DEFAULT_DELAY_MS


_GENERATORS = {
    "healthy_light": _tr_iid(0.01, 0.01),
    "alarm_outage": _tr_alarm_outage,
    "burst_loss": _tr_burst_loss,
    "ack_loss": _tr_iid(0.01, 0.40),
    "reorder": _tr_reorder,
    "overflow": _tr_overflow,
    "overload": _tr_iid(0.01, 0.01),
    "tight_deadline": _tr_tight_deadline,
}


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def generate_trace(scenario, seed):
    """Return the trace rows for (scenario, seed): a list of exactly
    run_ms // slot_ms 5-tuples (slot, data_lost, data_delay_ms, ack_lost,
    ack_delay_ms) for slots 0..n-1. Raises ValueError for an unknown
    scenario."""
    if scenario not in SCENARIOS:
        raise ValueError("unknown scenario: %s" % scenario)
    cfg = SCENARIOS[scenario]
    run_ms = cfg["run_ms"]
    slot_ms = cfg["slot_ms"]
    n_slots = run_ms // slot_ms
    rng = random.Random(seed)
    gen = _GENERATORS[scenario]
    state = {}
    rows = []
    for k in range(n_slots):
        t = k * slot_ms
        dl, dd, al, ad = gen(rng, k, t, state)
        if dl not in (0, 1) or al not in (0, 1) or dd < 1 or ad < 1:
            raise AssertionError("bad trace row at slot %d" % k)
        rows.append((k, dl, dd, al, ad))
    return rows


def write_trace_csv(rows, fp):
    w = csv.writer(fp, lineterminator="\n")
    w.writerow(["slot", "data_lost", "data_delay_ms", "ack_lost", "ack_delay_ms"])
    for r in rows:
        w.writerow(list(r))


def main(argv=None):
    p = argparse.ArgumentParser(description="Freshness Lab seeded channel trace generator (HOST SIMULATION inputs)")
    p.add_argument("--scenario", help="scenario name (see --list)")
    p.add_argument("--seed", type=int, help="integer seed for random.Random")
    p.add_argument("--out", help="output trace CSV path ('-' for stdout)")
    p.add_argument("--list", action="store_true", help="print scenario names and exit")
    args = p.parse_args(argv)
    if args.list:
        for name in SCENARIO_NAMES:
            print(name)
        return 0
    if args.scenario is None or args.seed is None or args.out is None:
        p.error("--scenario, --seed and --out are required (or --list)")
    if args.scenario not in SCENARIOS:
        p.error("unknown scenario %r; use --list" % args.scenario)
    rows = generate_trace(args.scenario, args.seed)
    if args.out == "-":
        write_trace_csv(rows, sys.stdout)
    else:
        with open(args.out, "w", newline="") as fp:
            write_trace_csv(rows, fp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
