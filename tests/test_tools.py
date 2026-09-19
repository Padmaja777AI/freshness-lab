"""Python tool tests (stdlib unittest). HOST SIMULATION tooling only.

Covers: generator determinism and CSV validity per docs/DESIGN.md §8.2/§8.4/§10,
the pure-Python restatement of the continuous-time AoI metric (§8.6), the
summarize aggregation math, the counterexample reducer's ddmin, and the SVG
plot output. Modules written by other steps are imported lazily and skipped
with a clear message if absent, so the suite always runs.
"""
import importlib
import os
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))


def _import(name):
    try:
        return importlib.import_module(name)
    except ModuleNotFoundError as e:  # pragma: no cover - depends on build progress
        raise unittest.SkipTest(f"tools/{name}.py not present yet: {e}")


def aoi_continuous(samples, end, threshold):
    """Continuous-time AoI over [first_rx, end) for (gen, rx) samples (§8.6).

    Returns (area, mean, peak, final_age, over_threshold_time, unknown).
    Segment of length n starting at age a0 contributes n*a0 + n*n/2.
    """
    samples = sorted(samples, key=lambda s: s[1])
    first_rx = samples[0][1]
    area = 0.0
    peak = 0.0
    over = 0.0
    for i, (gen, rx) in enumerate(samples):
        seg_end = samples[i + 1][1] if i + 1 < len(samples) else end
        n = seg_end - rx
        a0 = rx - gen
        area += n * a0 + n * n / 2.0
        peak = max(peak, a0 + n)
        if a0 >= threshold:
            over += n
        elif a0 + n > threshold:
            over += a0 + n - threshold
    final_age = end - samples[-1][0]
    return area, area / (end - first_rx), peak, final_age, over, first_rx


class AoiFixture(unittest.TestCase):
    def test_hand_fixture(self):
        # (0,0),(2,3),(6,8), end 10: 4.5 + 17.5 + 6 = 28; mean 2.8; peak 6; final 4.
        area, mean, peak, final, over, unknown = aoi_continuous([(0, 0), (2, 3), (6, 8)], 10, 3)
        self.assertAlmostEqual(area, 28.0)
        self.assertAlmostEqual(mean, 2.8)
        self.assertEqual(peak, 6)
        self.assertEqual(final, 4)
        self.assertEqual(unknown, 0)
        # threshold 3: (6-3) from [3,8) plus (4-3) from [8,10) = 4
        self.assertAlmostEqual(over, 4.0)
        # the discrete left-sample sum would be 23; make sure that is NOT what we compute
        self.assertNotAlmostEqual(area, 23.0)

    def test_late_first_reception_changes_denominator(self):
        # same samples but the first reception at t=5: unknown 5, mean over 5 ms only
        area, mean, _, _, _, unknown = aoi_continuous([(5, 5)], 10, 100)
        self.assertEqual(unknown, 5)
        self.assertAlmostEqual(area, 12.5)
        self.assertAlmostEqual(mean, 2.5)


class Generators(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gw = _import("gen_workload")
        cls.gt = _import("gen_trace")

    def _rows(self, scenario, seed):
        return self.gw.generate_workload(scenario, seed), self.gt.generate_trace(scenario, seed)

    def test_deterministic_same_seed(self):
        for sc in self.gw.SCENARIOS:
            w1, t1 = self._rows(sc, 101)
            w2, t2 = self._rows(sc, 101)
            self.assertEqual(w1, w2, sc)
            self.assertEqual(t1, t2, sc)

    def test_different_seeds_differ_for_seeded_scenarios(self):
        w1, t1 = self._rows("healthy_light", 101)
        w2, t2 = self._rows("healthy_light", 102)
        self.assertNotEqual(w1, w2)
        self.assertNotEqual(t1, t2)

    def test_alarm_outage_stream0_and_events_seed_independent(self):
        w1, t1 = self._rows("alarm_outage", 101)
        w2, t2 = self._rows("alarm_outage", 102)
        s0_1 = [r for r in w1 if r[1] != "STATE" or int(r[2]) == 0]
        s0_2 = [r for r in w2 if r[1] != "STATE" or int(r[2]) == 0]
        self.assertEqual(s0_1, s0_2)
        self.assertEqual(t1, t2)
        ev = [r for r in w1 if r[1] in ("RAISE", "CLEAR")]
        self.assertEqual([(int(r[0]), r[1]) for r in ev], [(10000, "RAISE"), (12000, "CLEAR")])
        # the alarm state stream shows 1 only inside [10000, 12000)
        s0 = [(int(r[0]), int(r[3])) for r in w1 if r[1] == "STATE" and int(r[2]) == 0]
        for t, v in s0:
            self.assertEqual(v, 1 if 10000 <= t < 12000 else 0, (t, v))
        self.assertIn((10000, 1), s0)
        self.assertIn((12000, 0), s0)

    def test_csv_validity_all_scenarios(self):
        for sc, cfg in self.gw.SCENARIOS.items():
            run_ms, slot_ms = int(cfg["run_ms"]), int(cfg["slot_ms"])
            w, t = self._rows(sc, 101)
            times = [int(r[0]) for r in w]
            self.assertEqual(times, sorted(times), sc)
            self.assertTrue(all(0 <= x < run_ms for x in times), sc)
            self.assertEqual(len(t), run_ms // slot_ms, sc)
            for k, row in enumerate(t):
                self.assertEqual(int(row[0]), k, sc)
                self.assertIn(int(row[1]), (0, 1), sc)
                self.assertIn(int(row[3]), (0, 1), sc)
                self.assertGreaterEqual(int(row[2]), 1, sc)
                self.assertGreaterEqual(int(row[4]), 1, sc)
            for r in w:
                self.assertIn(r[1], ("STATE", "RAISE", "CLEAR"), sc)
                if r[1] == "STATE":
                    self.assertLess(int(r[2]), int(cfg["n_streams"]), sc)
                else:
                    self.assertLessEqual(int(r[3]), int(r[4]), sc)  # deadline_rel <= retention_rel

    def test_alarm_outage_trace_window(self):
        _, t = self._rows("alarm_outage", 101)
        for row in t:
            k = int(row[0])
            expect = 1 if 900 <= k < 1500 else 0
            self.assertEqual(int(row[1]), expect, k)
            self.assertEqual(int(row[3]), expect, k)

    def test_scenario_shapes(self):
        w, _ = self._rows("overflow", 101)
        self.assertEqual(sum(1 for r in w if r[1] == "RAISE"), 20)
        self.assertEqual(sum(1 for r in w if r[1] == "CLEAR"), 20)
        w, _ = self._rows("overload", 101)
        self.assertEqual(sum(1 for r in w if r[1] == "RAISE"), 2500)
        w, _ = self._rows("tight_deadline", 101)
        self.assertEqual(len({int(r[2]) for r in w if r[1] == "STATE"}), 8)
        w, _ = self._rows("healthy_light", 101)
        self.assertEqual(sum(1 for r in w if r[1] == "RAISE"), 6)
        self.assertEqual(sum(1 for r in w if r[1] == "CLEAR"), 6)


class Summarize(unittest.TestCase):
    def test_aggregate_mean_min_max(self):
        sm = _import("summarize")
        rows = [
            {"scenario": "s", "policy": "p", "seed": "1", "recall": "0.5"},
            {"scenario": "s", "policy": "p", "seed": "2", "recall": "1.0"},
            {"scenario": "s", "policy": "p", "seed": "3", "recall": "0.25"},
            {"scenario": "s", "policy": "q", "seed": "1", "recall": "0.75"},
        ]
        header, out = sm.aggregate(rows, metrics=["recall"])
        self.assertIn("recall_mean", header)
        by = {(r["scenario"], r["policy"]): r for r in out}
        p = by[("s", "p")]
        self.assertEqual(p["n_seeds"], "3")
        self.assertAlmostEqual(float(p["recall_mean"]), (0.5 + 1.0 + 0.25) / 3, places=5)  # printed with 6 decimals
        self.assertEqual(p["recall_min"], "0.25")
        self.assertEqual(p["recall_max"], "1.0")
        self.assertEqual(by[("s", "q")]["n_seeds"], "1")


class Reducer(unittest.TestCase):
    def test_ddmin_finds_minimal_pair(self):
        rc = _import("reduce_counterexample")
        calls = []

        def pred(items):
            calls.append(list(items))
            return 3 in items and 7 in items

        result = rc.ddmin(list(range(20)), pred)
        self.assertEqual(sorted(result), [3, 7])
        self.assertTrue(all(len(c) > 0 for c in calls), "predicate must never be called on the empty list")


class Plot(unittest.TestCase):
    def test_svg_parses_and_is_labelled(self):
        ps = _import("plot_svg")
        rows = []
        for sc in ("healthy_light", "alarm_outage"):
            for pol, ot, aoi in (("edf_rr", "0.9", "500"), ("fresh", "0.8", "450")):
                rows.append({
                    "scenario": sc, "policy": pol, "n_seeds": "2", "seeds": "101;102",
                    "on_time_rate_mean": ot, "on_time_rate_min": ot, "on_time_rate_max": ot,
                    "aoi_mean_ms_mean": aoi, "aoi_mean_ms_min": aoi, "aoi_mean_ms_max": aoi,
                })
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "p.svg")
            ps.write_plot(rows, path)
            with open(path, encoding="utf-8") as fp:
                text = fp.read()
            ET.fromstring(text)
            self.assertIn("HOST SIMULATION", text)


if __name__ == "__main__":
    unittest.main()
