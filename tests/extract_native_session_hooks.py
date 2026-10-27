"""Compile the actual desktop/native startup gate in the lifecycle fixture."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("if(runtimeSession && !runtimeStopped && !nativeHooksInstalled")
body = source.index("{", start)
depth = 1
end = body + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
Path(sys.argv[2]).write_text(source[start:end] + "\n", encoding="utf-8")
