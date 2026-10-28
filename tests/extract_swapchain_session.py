"""Exercise the production suspension functions, not a copied state machine."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
functions = []
for name in ("SuspendSession", "ResumeSession", "SuspendForSwapchainChange"):
    start = source.index(f"bool {name}() {{")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    functions.append(source[start:end])
Path(sys.argv[2]).write_text("\n".join(functions) + "\n", encoding="utf-8")
