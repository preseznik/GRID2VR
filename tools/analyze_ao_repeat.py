"""Compare an immediate native AO draw repeat and verify target restoration."""
import json
import re
from analyze_engine_trace import fields
from analyze_replay_work import compute_resource_pair


def analyze(run, producer="ao", render_pass=None):
    if producer not in ("ao", "shadow"):
        raise ValueError("Unknown repeat producer")
    if (run / "capture-cleanup.json").exists():
        raise ValueError("Draw control captures were retired; use the retained summary.")
    trace = run / "trace.log"
    lines = trace.read_text().splitlines() if trace.exists() else []
    rows = [fields(line) for line in lines if producer + "_repeat name=" in line]
    if any(not re.fullmatch(r"work-\d+-pass[01]-slot0-draw\d+", row.get("name", "")) for row in rows):
        raise ValueError("Unknown producer repeat draw identity")
    if render_pass is not None:
        rows = [row for row in rows if f"-pass{render_pass}-" in row["name"]]
    result = dict(complete=False, repeatedIdentically=False, restored=False, rows=rows)
    if len(rows) != 1 or rows[0].get("repeated") != "1" or rows[0].get("restore_requested") != "1" or rows[0].get("injected_draws") != "1":
        return result
    name = rows[0]["name"]
    original = run / (name + "-repeat-original.texture")
    control = compute_resource_pair(original, run / (name + "-repeat-control.texture"))
    restored = compute_resource_pair(original, run / (name + "-repeat-restored.texture"))
    result.update(complete=bool(control.get("valid") and restored.get("valid")),
                  repeatedIdentically=control.get("equal") is True, restored=restored.get("equal") is True,
                  control=control, restoration=restored,
                  limits=f"One guarded {producer} draw repeated immediately from the same saved incoming target. The original result is restored. This is an injected GPU draw, not another engine/simulation update.")
    return result
