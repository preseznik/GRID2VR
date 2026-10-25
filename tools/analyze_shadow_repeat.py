"""Compare an immediate shadow-mask draw repeat and verify color restoration."""
from analyze_ao_repeat import analyze as analyze_repeat
from analyze_replay_work import compute_resource_pair


def analyze(run):
    if not (run / "normalize-shadow-bindings.request").exists():
        return analyze_repeat(run, "shadow")
    passes = [analyze_repeat(run, "shadow", p) for p in (0, 1)]
    names = [p["rows"][0]["name"] if len(p["rows"]) == 1 else "missing" for p in passes]
    identity = names[0].replace("-pass0-", "-pass1-") == names[1]
    normalized = compute_resource_pair(*(run / (name + "-repeat-original.texture") for name in names))
    native = compute_resource_pair(*(run / (name + "-repeat-control.texture") for name in names))
    complete = bool(identity and all(p["complete"] for p in passes) and normalized.get("valid") and native.get("valid"))
    bindings = complete and all(p["rows"][0].get(flag) == "1" for p in passes
                               for flag in ("binding_control", "native_applied", "normalized_restored"))
    return dict(complete=complete, restored=bool(bindings and all(p["restored"] for p in passes)),
                repeatedIdentically=all(p["repeatedIdentically"] for p in passes), passes=passes,
                normalizedPair=normalized, nativePair=native,
                limits="Same captured scene: native versus cleared unused shadow bindings in each pass, from that pass's saved incoming target. Both target and binding restoration are required. This does not certify other draws or simulation state.")
