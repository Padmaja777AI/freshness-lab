#!/usr/bin/env python3
"""
Freshness Lab -- HOST SIMULATION matrix chart as a pure-stdlib SVG.

    python3 tools/plot_svg.py --in OUT/summary_by_scenario.csv --out OUT/plot.svg

Importable: write_plot(summary_by_scenario_rows, path, title=None) -> svg text.

Two panels of grouped bars (one group per scenario, one bar per policy):
  A. on_time_rate (fixed 0..1 axis), mean over seeds, min/max whiskers
  B. aoi_mean_ms, mean over seeds, min/max whiskers

Design notes (dataviz skill): colour follows the policy (a fixed slot per
policy name, never re-assigned by rank or filter) using the colourblind-
validated categorical order of the reference palette (adjacent-pair CVD
delta-E >= 8 in both modes, validated with the palette script). A legend is
always drawn, every bar carries a <title> tooltip, and the values live in
summary_by_scenario.csv / summary.md (the table view) because three light-mode
hues sit below 3:1 contrast. Text uses ink tokens, never series colours. Bars
are thin (14px) with a 2px surface gap and a 4px rounded data end; gridlines
are solid hairlines. Dark mode is a selected set of steps applied through a
prefers-color-scheme media query; presentation attributes carry the light
values for renderers without CSS support. Long title/subtitle/footer text is
word-wrapped to the chart width (estimated glyph widths) and the chart never
narrows below MIN_WIDTH, so no text overflows the viewBox on a 1-2 scenario
run. Standard library only.
"""
from __future__ import annotations

import argparse
import csv
import math
import sys
from xml.sax.saxutils import escape

# Categorical slots (reference palette, fixed order = CVD-safety mechanism).
SLOT_LIGHT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948"]
SLOT_DARK = ["#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#008300", "#9085e9", "#e66767"]
# Colour follows the entity: each policy name owns one slot for good.
POLICY_SLOT = {
    "edf_rr": 0,
    "edf_rr_ld": 1,
    "fresh_nodefer": 2,
    "fresh": 3,
    "fresh_ld": 4,
    "fresh_so": 5,
    "fifo": 6,
    "fresh_nodefer_ld": 7,
}
OTHER_FILL = "#898781"  # any policy outside the fixed map: neutral, named in the legend

INK = {  # role: (light, dark)
    "surface": ("#fcfcfb", "#1a1a19"),
    "primary": ("#0b0b0b", "#ffffff"),
    "secondary": ("#52514e", "#c3c2b7"),
    "muted": ("#898781", "#898781"),
    "grid": ("#e1e0d9", "#2c2c2a"),
    "axis": ("#c3c2b7", "#383835"),
}
FONT = "system-ui, -apple-system, 'Segoe UI', sans-serif"

BAR_W = 14
BAR_GAP = 2
GROUP_PAD = 14
MIN_GROUP_W = 104
END_RADIUS = 4
PLOT_H = 200
X_BAND = 34
PANEL_TITLE_H = 26
PANEL_GAP = 30
ML, MR = 76, 24
TITLE_LINE_H = 22
SUBTITLE_LINE_H = 16
LEGEND_ROW_H = 22
FOOT_LINE_H = 15
MIN_WIDTH = 760

PANELS = [
    ("A", "on_time_rate", "On-time rate = rx_ev_on_time / ev_generated (0..1)", True),
    ("B", "aoi_mean_ms", "Mean AoI at the receiver (ms, time-weighted, averaged over streams)", False),
]
DEFAULT_TITLE = (
    "HOST SIMULATION -- exploratory/pilot scheduler matrix (development seeds; not a held-out "
    "evaluation): on-time rate and mean AoI by scenario and policy"
)
FOOT_LINES = [
    "HOST SIMULATION on a desktop CPU (docs/DESIGN.md scope label); no microcontroller board has run this "
    "code. Same workload, config and matched channel trace for every policy within a scenario x seed.",
    "Whiskers = min/max over seeds. Colour identifies the policy (fixed slot per name); the legend, the "
    "tooltips and the tables carry the same identity and values. Bytes are not energy.",
]


def _f(x):
    return f"{x:.2f}".rstrip("0").rstrip(".") if isinstance(x, float) else str(x)


def nice_axis(vmax):
    """Top-of-axis and tick step with 'clean' numbers (1/2/2.5/5 x 10^k)."""
    if vmax <= 0:
        return 1.0, 0.25
    raw = vmax / 4.0
    mag = 10 ** math.floor(math.log10(raw))
    step = mag
    for f in (1, 2, 2.5, 5, 10):
        step = f * mag
        if vmax / step <= 5:
            break
    top = math.ceil(vmax / step - 1e-9) * step
    return float(top), float(step)


def fmt_tick(v):
    if abs(v - round(v)) < 1e-9:
        return f"{int(round(v)):,}"
    return f"{v:g}"


def policy_fill(policy):
    slot = POLICY_SLOT.get(policy)
    if slot is None:
        return OTHER_FILL, OTHER_FILL, "sx"
    return SLOT_LIGHT[slot], SLOT_DARK[slot], f"s{slot}"


def bar_path(x, y_top, w, y_base, r=END_RADIUS):
    """Column growing from the baseline, rounded only at the data end."""
    h = y_base - y_top
    if h <= 0:
        return None
    rr = min(r, h, w / 2.0)
    return (
        f"M{_f(x)},{_f(y_base)} L{_f(x)},{_f(y_top + rr)} Q{_f(x)},{_f(y_top)} {_f(x + rr)},{_f(y_top)} "
        f"L{_f(x + w - rr)},{_f(y_top)} Q{_f(x + w)},{_f(y_top)} {_f(x + w)},{_f(y_top + rr)} "
        f"L{_f(x + w)},{_f(y_base)} Z"
    )


def _text_w(s, size=12):
    return len(s) * size * 0.58


def wrap_text(text, max_px, size):
    """Greedy word wrap using the glyph-width estimate; never returns an empty list."""
    max_chars = max(8, int(max_px / (size * 0.58)))
    lines, cur = [], ""
    for word in text.split():
        cand = word if not cur else cur + " " + word
        if len(cand) <= max_chars or not cur:
            cur = cand
        else:
            lines.append(cur)
            cur = word
    lines.append(cur)
    return lines


def _parse_rows(rows):
    scenarios, policies, data = [], [], {}
    for r in rows:
        sc, pol = r["scenario"], r["policy"]
        if sc not in scenarios:
            scenarios.append(sc)
        if pol not in policies:
            policies.append(pol)
        entry = {"n_seeds": int(float(r.get("n_seeds", "1") or 1))}
        for _, metric, _, _ in PANELS:
            entry[metric] = tuple(float(r[f"{metric}_{k}"]) for k in ("mean", "min", "max"))
        data[(sc, pol)] = entry
    return scenarios, policies, data


def render_svg(rows, title=None):
    """Return the SVG document text for summary_by_scenario rows."""
    title = title or DEFAULT_TITLE
    if "HOST SIMULATION" not in title:
        title = "HOST SIMULATION -- " + title
    scenarios, policies, data = _parse_rows(list(rows))
    n_sc, n_pol = len(scenarios), len(policies)

    bars_w = n_pol * (BAR_W + BAR_GAP) - BAR_GAP if n_pol else 0
    group_w = max(MIN_GROUP_W, bars_w + 2 * GROUP_PAD)
    plot_w = max(n_sc, 1) * group_w
    width = max(MIN_WIDTH, ML + plot_w + MR)
    plot_w = width - ML - MR  # groups spread over the full plot width
    group_w = plot_w / max(n_sc, 1)

    # legend layout (wrapping rows)
    legend_items = []
    x = ML
    row_i = 0
    for pol in policies:
        w = 18 + _text_w(pol) + 18
        if x + w > ML + plot_w and x > ML:
            row_i += 1
            x = ML
        legend_items.append((pol, x, row_i))
        x += w
    legend_rows = (row_i + 1) if policies else 0

    seeds_set = sorted({e["n_seeds"] for e in data.values()})
    if not seeds_set:
        n_txt = "no runs"
    elif len(seeds_set) == 1:
        n_txt = f"n = {seeds_set[0]} seed{'s' if seeds_set[0] != 1 else ''}"
    else:
        n_txt = f"n = {seeds_set[0]}..{seeds_set[-1]} seeds"

    subtitle = (
        f"Bars: mean over seeds per scenario x policy ({n_txt}); whiskers: min to max across seeds. "
        "Development seeds, not a held-out evaluation. Bytes are not energy; no superiority claim. "
        "Values: summary_by_scenario.csv (table view)."
    )
    title_lines = wrap_text(title, plot_w, 16)
    subtitle_lines = wrap_text(subtitle, plot_w, 12)
    foot_lines = []
    for f in FOOT_LINES:
        foot_lines.extend(wrap_text(f, plot_w, 11))
    title_h = len(title_lines) * TITLE_LINE_H
    subtitle_h = len(subtitle_lines) * SUBTITLE_LINE_H + 4
    mt = 12 + title_h + subtitle_h + legend_rows * LEGEND_ROW_H + 8
    panel_h = PANEL_TITLE_H + PLOT_H + X_BAND
    foot_h = 20 + len(foot_lines) * FOOT_LINE_H + 8
    height = mt + len(PANELS) * panel_h + (len(PANELS) - 1) * PANEL_GAP + foot_h

    L = INK
    out = []
    out.append('<?xml version="1.0" encoding="UTF-8"?>')
    out.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="fl-title fl-desc">'
    )
    out.append(f'<title id="fl-title">{escape(title)}</title>')
    out.append(
        '<desc id="fl-desc">HOST SIMULATION on a desktop CPU; no board, no real link. Grouped bars per '
        "scenario (one bar per policy): panel A on-time rate, panel B mean AoI in ms; bars are means over "
        "seeds, whiskers are min to max across seeds. Every value is listed in summary_by_scenario.csv and "
        "summary.md. Bytes are not energy; no superiority claim.</desc>"
    )
    style = [
        "<style>",
        f"text {{ font-family: {FONT}; }}",
        "@media (prefers-color-scheme: dark) {",
        f"  .surface {{ fill: {L['surface'][1]}; }}",
        f"  .ink {{ fill: {L['primary'][1]}; }}",
        f"  .ink2 {{ fill: {L['secondary'][1]}; }}",
        f"  .muted {{ fill: {L['muted'][1]}; }}",
        f"  .grid {{ stroke: {L['grid'][1]}; }}",
        f"  .axis {{ stroke: {L['axis'][1]}; }}",
        f"  .whisk {{ stroke: {L['secondary'][1]}; }}",
    ]
    for i, c in enumerate(SLOT_DARK):
        style.append(f"  .s{i} {{ fill: {c}; }}")
    style.append("}")
    style.append("</style>")
    out.extend(style)
    out.append(f'<rect class="surface" x="0" y="0" width="{width}" height="{height}" fill="{L["surface"][0]}"/>')
    for i, line in enumerate(title_lines):
        out.append(
            f'<text class="ink" x="{ML}" y="{12 + 18 + i * TITLE_LINE_H}" font-size="16" font-weight="600" '
            f'fill="{L["primary"][0]}">{escape(line)}</text>'
        )
    for i, line in enumerate(subtitle_lines):
        out.append(
            f'<text class="ink2" x="{ML}" y="{12 + title_h + 12 + i * SUBTITLE_LINE_H}" font-size="12" '
            f'fill="{L["secondary"][0]}">{escape(line)}</text>'
        )
    # legend
    ly0 = 12 + title_h + subtitle_h + 4
    for pol, lx, lrow in legend_items:
        light, _, cls = policy_fill(pol)
        y = ly0 + lrow * LEGEND_ROW_H
        out.append(f'<rect class="{cls}" x="{lx}" y="{y + 3}" width="12" height="12" rx="2" fill="{light}"/>')
        out.append(
            f'<text class="ink2" x="{lx + 18}" y="{y + 13}" font-size="12" fill="{L["secondary"][0]}">'
            f"{escape(pol)}</text>"
        )

    if not data:
        out.append(
            f'<text class="muted" x="{ML}" y="{mt + 40}" font-size="13" fill="{L["muted"][0]}">'
            "No runs to plot.</text>"
        )
        out.append("</svg>")
        return "\n".join(out) + "\n"

    y_cursor = mt
    for label, metric, caption, fixed_unit in PANELS:
        py0 = y_cursor + PANEL_TITLE_H  # top of plot area
        base = py0 + PLOT_H  # baseline
        if fixed_unit:
            top, step = 1.0, 0.25
        else:
            vmax = max(data[k][metric][2] for k in data)
            top, step = nice_axis(vmax)

        def yv(v, top=top, py0=py0, base=base):
            return base - (v / top) * PLOT_H if top > 0 else base

        out.append(
            f'<text class="ink" x="{ML}" y="{y_cursor + 16}" font-size="13" font-weight="600" '
            f'fill="{L["primary"][0]}">{escape(label + ". " + caption)}</text>'
        )
        # gridlines and tick labels
        n_ticks = int(round(top / step)) if step > 0 else 0
        for i in range(n_ticks + 1):
            v = i * step
            y = yv(v)
            if i > 0:
                out.append(
                    f'<line class="grid" x1="{ML}" y1="{_f(y)}" x2="{ML + plot_w}" y2="{_f(y)}" '
                    f'stroke="{L["grid"][0]}" stroke-width="1"/>'
                )
            out.append(
                f'<text class="muted" x="{ML - 8}" y="{_f(y + 4)}" font-size="11" text-anchor="end" '
                f'fill="{L["muted"][0]}">{escape(fmt_tick(v))}</text>'
            )
        out.append(
            f'<line class="axis" x1="{ML}" y1="{base}" x2="{ML + plot_w}" y2="{base}" '
            f'stroke="{L["axis"][0]}" stroke-width="1"/>'
        )
        # bars
        for si, sc in enumerate(scenarios):
            gx = ML + si * group_w
            bx0 = gx + (group_w - bars_w) / 2.0
            for pi, pol in enumerate(policies):
                e = data.get((sc, pol))
                bx = bx0 + pi * (BAR_W + BAR_GAP)
                if e is None:
                    continue
                mean, lo, hi = e[metric]
                light, _, cls = policy_fill(pol)
                p = bar_path(bx, yv(mean), BAR_W, base)
                if p:
                    out.append(f'<path class="{cls}" d="{p}" fill="{light}"/>')
                if hi > lo:
                    cx = bx + BAR_W / 2.0
                    y_lo, y_hi = yv(lo), yv(hi)
                    out.append(
                        f'<line class="whisk" x1="{_f(cx)}" y1="{_f(y_lo)}" x2="{_f(cx)}" y2="{_f(y_hi)}" '
                        f'stroke="{L["secondary"][0]}" stroke-width="1.5"/>'
                    )
                    for yy in (y_lo, y_hi):
                        out.append(
                            f'<line class="whisk" x1="{_f(cx - 3)}" y1="{_f(yy)}" x2="{_f(cx + 3)}" y2="{_f(yy)}" '
                            f'stroke="{L["secondary"][0]}" stroke-width="1.5"/>'
                        )
                tip = f"{sc} / {pol}: {metric} mean {mean:g} (min {lo:g}, max {hi:g}; n={e['n_seeds']} seeds)"
                out.append(
                    f'<rect x="{_f(bx - 1)}" y="{py0}" width="{BAR_W + 2}" height="{PLOT_H}" fill="#000" '
                    f'fill-opacity="0" pointer-events="all"><title>{escape(tip)}</title></rect>'
                )
            # long scenario names on a narrow group: truncate with an ellipsis and keep the full name as a tooltip
            if _text_w(sc, 11) <= group_w - 4:
                sc_label, sc_tip = sc, ""
            else:
                sc_label = sc[: max(3, int((group_w - 4) / (11 * 0.58)) - 1)] + "\u2026"
                sc_tip = f"<title>{escape(sc)}</title>"
            out.append(
                f'<text class="ink2" x="{_f(gx + group_w / 2.0)}" y="{base + 18}" font-size="11" '
                f'text-anchor="middle" fill="{L["secondary"][0]}">{sc_tip}{escape(sc_label)}</text>'
            )
        y_cursor += panel_h + PANEL_GAP

    foot_y = height - foot_h + 16
    for i, line in enumerate(foot_lines):
        out.append(
            f'<text class="muted" x="{ML}" y="{foot_y + i * FOOT_LINE_H}" font-size="11" fill="{L["muted"][0]}">'
            f"{escape(line)}</text>"
        )
    out.append("</svg>")
    return "\n".join(out) + "\n"


def write_plot(summary_by_scenario_rows, path, title=None):
    """Render the two-panel chart for summary_by_scenario rows and write it to path."""
    svg = render_svg(summary_by_scenario_rows, title=title)
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(svg)
    return svg


def read_rows(path):
    with open(path, newline="", encoding="utf-8") as fp:
        return list(csv.DictReader(fp))


def main(argv=None):
    ap = argparse.ArgumentParser(description="HOST SIMULATION matrix chart (pure-stdlib SVG).")
    ap.add_argument("--in", dest="inp", required=True, help="summary_by_scenario.csv")
    ap.add_argument("--out", required=True, help="output .svg path")
    ap.add_argument("--title", default=None, help="chart title (HOST SIMULATION is prefixed if absent)")
    args = ap.parse_args(argv)
    rows = read_rows(args.inp)
    write_plot(rows, args.out, title=args.title)
    print(f"wrote {args.out} ({len(rows)} scenario x policy rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
