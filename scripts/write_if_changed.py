"""Small deterministic-codegen helper used by PlatformIO pre-build scripts."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path
from typing import Union


def write_if_changed(path: Union[str, os.PathLike[str]], data: Union[str, bytes]) -> bool:
    """Atomically replace *path* only when its bytes changed."""
    target = Path(path)
    payload = data.encode("utf-8") if isinstance(data, str) else data
    try:
        if target.read_bytes() == payload:
            return False
    except FileNotFoundError:
        pass

    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{target.name}.", dir=target.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(payload)
        os.replace(temporary, target)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise
    return True
