#!/usr/bin/env python3
"""Extract PlatformIO's static memory measurements without rounding."""
import json
import re
import sys
from pathlib import Path


def parse_stats(log, environment, sha):
    result = {"environment": environment, "sha": sha}
    for label in ("RAM", "Flash"):
        # Nested SDK builds may also print sizes; use the final firmware result.
        matches = re.findall(rf"{label}:.*?used\s+(\d+)\s+bytes\s+from\s+(\d+)\s+bytes", log)
        result[label.lower()] = int(matches[-1][0]) if matches else None
    return result


if __name__ == "__main__":
    print(json.dumps(parse_stats(Path(sys.argv[1]).read_text(), sys.argv[2], sys.argv[3])))
