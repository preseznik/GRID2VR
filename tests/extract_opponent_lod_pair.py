"""Exercise the actual production eye-pair suppression/finally boundary."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("bool InvokeReplayPair(")
end = source.index("{", start) + 1
depth = 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
Path(sys.argv[2]).write_text(source[start:end] + "\n", encoding="utf-8")
