"""Run the AirDrop host checks, including sanitizer coverage, without device hardware."""

from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="crosspoint-airdrop-") as directory:
        for name in ("serve_test.c", "stream_test.cpp", "gauge_test.c", "ack_test.c"):
            cpp = name.endswith(".cpp")
            compiler = shlex.split(os.environ.get("CXX" if cpp else "CC", "c++" if cpp else "cc"))
            binary = Path(directory) / Path(name).stem
            subprocess.run(compiler + ["-std=c++17" if cpp else "-std=c11", "-g",
                           "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                           str(HERE / name), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        for name in ("ask_test.py", "mailbox_test.py", "content_length_test.py", "identity_test.py", "approval_test.py"):
            subprocess.run([sys.executable, str(HERE / name)], check=True)


if __name__ == "__main__":
    main()
