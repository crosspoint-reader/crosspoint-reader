"""Check recent-file filtering with a built simulator_picco (or other PSRAM simulator).

python3 test/test_library_recents.py --simulator .pio/build/simulator_picco/program --artifacts /path/to/results
"""

import argparse
import json
import os
from pathlib import Path
import subprocess


def run_case(simulator, artifacts, name, theme, current_crash, script, expected_first):
    root = artifacts / name
    cache = root / ".crosspoint"
    cache.mkdir(parents=True, exist_ok=True)
    paths = ["Alpha.txt", "crash_report.txt", "Beta.txt", "My Clippings.txt", "Gamma.txt"]
    if current_crash:
        paths = ["crash_report.txt", "Alpha.txt", "My Clippings.txt", "Beta.txt", "Gamma.txt"]
    for path in paths:
        (root / path).write_text(path + "\n\nSimulator regression fixture.\n")
    recent = {"books": [{"path": "/" + p, "title": p[:-4], "author": "", "coverBmpPath": ""} for p in paths]}
    (cache / "recent.json").write_text(json.dumps(recent))
    (cache / "state.json").write_text(json.dumps({"openEpubPath": "", "showBootScreen": False}))
    (cache / "settings.json").write_text(json.dumps({"uiTheme": theme, "orientation": 0}))
    env = dict(os.environ, CROSSPOINT_SIM_SD=str(root), CROSSPOINT_SIM_SCREEN_TEXT="1",
               CROSSPOINT_SIM_INPUT_SCRIPT=script, SDL_VIDEODRIVER="dummy")
    log = artifacts / (name + ".log")
    with log.open("w") as output:
        result = subprocess.run([str(simulator)], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=90)
    assert result.returncode == 0, f"{name}: simulator failed; see {log}"
    screens = [line for line in log.read_text(errors="replace").splitlines() if "SCREEN:" in line]
    library_screens = [line for line in screens if "| Library | Recent | Title | Author |" in line]
    if theme == 1:
        assert library_screens, f"{name}: no Library screen; see {log}"
        assert all("crash_report" not in line and "My Clippings" not in line for line in library_screens), log
        assert "Alpha | Beta | Gamma | 1/3 books" in library_screens[0], library_screens[0]
    if current_crash:
        assert any("SCREEN: 100% | crash_report" in line for line in screens), log
    saved_paths = [book["path"] for book in json.loads((cache / "recent.json").read_text())["books"]]
    assert saved_paths[0] == expected_first, saved_paths
    assert "/crash_report.txt" in saved_paths and "/My Clippings.txt" in saved_paths, saved_paths
    if name == "library-remove":
        assert "/Beta.txt" not in saved_paths, saved_paths
        assert all((root / path).is_file() for path in paths), "Remove from recents deleted a file"
        assert any("Alpha | Gamma | Beta" in line for line in library_screens), log
    else:
        assert set(saved_paths) == {"/" + path for path in paths}, saved_paths
        assert any("SCREEN: Beta.txt |" in line for line in screens), log
    print(f"PASS {name}: {log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True, help="New directory for isolated SD roots and logs")
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=False)
    library = ("0:WAIT:Entering activity: Home;250:DOWN;500:DOWN;750:ENTER;"
               "750:WAIT:SCREEN:.*Library.*Alpha.*Beta.*Gamma;250:DOWN;500:DOWN;")
    opened = "750:ENTER;750:WAIT:SCREEN: Beta.txt;250:QUIT"
    removed = ("750:ENTER:1200;750:WAIT:SCREEN:.*Beta.*Open.*Remove from Recents;"
               "250:DOWN;500:ENTER;500:WAIT:Entering activity: Confirmation;"
               "250:DOWN;500:ENTER;500:WAIT:SCREEN:.*Library.*Alpha.*Gamma.*Beta;250:QUIT")
    grid = "0:WAIT:SCREEN: 100% | Alpha;250:DOWN;500:ENTER;500:WAIT:SCREEN: Beta.txt;250:QUIT"
    current = ("0:WAIT:SCREEN: 100% | crash_report;250:DOWN;500:DOWN;750:ENTER;"
               "750:WAIT:SCREEN: Beta.txt;250:QUIT")
    run_case(args.simulator.resolve(), artifacts, "library-open", 1, False, library + opened, "/Beta.txt")
    run_case(args.simulator.resolve(), artifacts, "library-current-crash", 1, True, library + opened, "/Beta.txt")
    run_case(args.simulator.resolve(), artifacts, "library-remove", 1, False, library + removed, "/Alpha.txt")
    run_case(args.simulator.resolve(), artifacts, "grid-recents", 4, False, grid, "/Beta.txt")
    run_case(args.simulator.resolve(), artifacts, "grid-current-crash", 4, True, current, "/Beta.txt")


if __name__ == "__main__":
    main()
