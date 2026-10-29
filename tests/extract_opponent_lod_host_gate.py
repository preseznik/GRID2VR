"""Compile production host gates in a no-game/no-XR owned-state fixture."""
from pathlib import Path
import sys


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


engine = Path(sys.argv[1]).read_text(encoding="utf-8")
diagnostics = Path(sys.argv[2]).read_text(encoding="utf-8")
present = function(diagnostics, "HRESULT STDMETHODCALLTYPE Present(")
# Exercise the actual gates below AND guard their production wiring/order.
assert present.count("PublishOpponentWorld();") == 1
publication = present.index("PublishOpponentWorld();")
assert publication > present.rindex("PollOpponentXr();")
assert publication > present.rindex("ShutdownOpponentXr();")
assert publication > present.rindex("engine::SuspendSession()")
assert publication > present.rindex("MirrorPreview(chain,f);")
assert publication < present.index("const auto hr=realPresent(")
assert present.index("opponentlod::host::RevokeWorld();") < present.index("DXGI_PRESENT_TEST")
assert "runtime::Tick(opponentWorld)" not in present
assert diagnostics.count("xrSession.Poll();") == 1
assert diagnostics.count("xrSession.Shutdown();") == 1
for signature in ("void PrepareSwapchainChange(", "HRESULT STDMETHODCALLTYPE Resize(",
                  "HRESULT STDMETHODCALLTYPE CreateSwapchain(", "void Attach(",
                  "bool ResetEyeHistory()", "const char* BeforeGameExit()"):
    body = function(diagnostics, signature)
    assert body.index("opponentlod::host::RevokeWorld();") < body.index(";"), signature
consumer = function(engine, "void __fastcall VisibilityConsumer(")
assert consumer.index("opponentVisibilityFrame=frame.load();") > consumer.index("realVisibilityConsumer(")
assert "qualified=valid && !inReplay" in consumer
alternate = function(engine, "uintptr_t __fastcall VisibilityJobReset(")
assert alternate.index("opponentVisibilityFrame=frame.load();") > alternate.index("PrepareVisibility(valid,")
pair = function(engine, "bool InvokeReplayPair(")
assert pair.index("SetEyeReplay(true)") < pair.index("for (")
assert pair.index("SetEyeReplay(false)") > pair.index("__finally")

engine_signatures = ("bool NativeWorldReady()", "bool OpponentLodWorldReady()", "bool Continuous()",
                     "bool SessionRendering()", "bool SessionFailed()", "void FailSession()",
                     "bool SuspendSession()", "bool ResumeSession()", "bool SuspendForSwapchainChange()",
                     "bool StopSession()")
diagnostic_signatures = ("bool OpponentXrReady() noexcept", "void PollOpponentXr()",
                        "void ShutdownOpponentXr()", "void PublishOpponentWorld() noexcept")
output = "#ifdef GRID2VR_HOST_GATE_ENGINE\n" + "\n".join(function(engine, s) for s in engine_signatures)
output += "\n#endif\n#ifdef GRID2VR_HOST_GATE_DIAGNOSTICS\n"
output += "\n".join(function(diagnostics, s) for s in diagnostic_signatures) + "\n#endif\n"
Path(sys.argv[3]).write_text(output, encoding="utf-8")
print("PASS: production Present order, immediate invalidation, native visibility roots and whole-eye SEH guard wiring")
