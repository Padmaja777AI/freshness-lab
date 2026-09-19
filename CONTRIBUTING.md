# Contributing

This is a small, software-only research project. The most useful
contributions are reproducible bug reports, behaviour changes that come
with hand-computed tests, and corrections that keep the documentation
truthful.

## Reproducing a bug

A behaviour of the core or harness is fully determined by four inputs:
scenario config, workload, channel trace and policy. To report one:

1. Name the scenario and seed (`python3 tools/gen_workload.py --list`), or
   attach the exact `workload.csv`, `trace.csv` and `.cfg` if hand-written.
2. Give the `flsim` command line including `--policy` and any
   `--defer`, `--late-demote`, `--starvation-override` overrides.
3. Attach `summary.csv`, `events.csv` and the relevant slots of
   `decisions.csv` and `rx.csv` from the run (all under an `out/` directory,
   never edited by hand).
4. If the problem is "policy A does worse than policy B", run
   `tools/reduce_counterexample.py` on it and attach `reduced_workload.csv`
   and `walkthrough.md`; a 1-minimal case is far easier to reason about.
5. State what you expected and cite the rule in `docs/DESIGN.md` it should
   follow. A disagreement between the code and DESIGN.md is a bug in one of
   them; say which you believe.

## Validating a behaviour change

* Update `docs/DESIGN.md` first if the rule changes; the spec is normative.
* Add or adjust tests with **hand-computed expectations** (times, counts,
  byte lengths, areas), not values copied from a run. Put the arithmetic in
  a comment above the test, as the existing suites do.
* Run all of: `make test`, `make sanitize`, `make clang-check`,
  `make tools-test`. Everything must stay at zero failures under `-Werror`.
* Keep the invariants: `fresh --defer 0` must remain byte-identical to
  `fresh_nodefer`; the accounting identity and the frame conservation check
  must hold on every run; the policy module must not gain access to receiver
  or channel state.
* If the change alters any output, regenerate the matrix into `out/matrix`
  and compare with `results/matrix/manifest.json`. Expected differences must
  be explained in the pull request and in `docs/REPORT.md`; unexpected ones
  are a finding to investigate before merging.

## Preserving baselines and evidence

* Never edit files under `results/` by hand. Regenerate them from a
  committed code state, and commit the regenerated files together with the
  updated `manifest.json` (which records `git_head`, the tracked-dirty flag
  and the binary hash) and a refreshed `results/reproduction_log.txt`.
* Keep `results/pilot/` and the seed-provenance notes in
  `docs/DESIGN.md §10.1`; negative and superseded results are part of the
  record.
* Scenario configurations apply identically to every policy. A change to a
  scenario's retry budget, capacities or candidate parameters is a change
  to the experiment and must be stated as such.
* Seeds 101..105 are development seeds. An unseen-seed evaluation must
  freeze the policies and configurations first and audit that the new seeds
  were not used during development.

## Keeping claims accurate

* Label every measurement HOST SIMULATION. There are no hardware
  measurements in this project and hardware is outside its scope.
* Report the candidate's results whether it wins or loses; do not remove a
  scenario because the result is inconvenient.
* Do not add superiority, novelty, affiliation, certification or version
  claims. Prior art is credited in `docs/RELATED_WORK.md`; no code is
  copied from the cited specifications.
* AI assistance is disclosed: the existing work was written and executed by
  Claude Code in this repository under human direction and review. Keep commit
  attribution honest (`Co-Authored-By` trailers for AI-assisted commits) and
  do not put model identifiers into code, comments or artefacts.
