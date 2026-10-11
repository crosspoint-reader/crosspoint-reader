#!/usr/bin/env python3
"""Build actual firmware sources for a static browser preview (Emscripten 6.0.6)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from sample_book import seed_card
from package_standalone import package

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
EMSDK_VERSION = "6.0.6"


def run(args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def ninja_path(path):
    return str(path).replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def browser_simulator(source, target):
    """Overlay browser task adapters, including headers reached by quoted includes."""
    for path in source.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(source)
        override = HERE / "runtime" / relative
        content = (override if override.is_file() else path).read_bytes()
        if relative.as_posix() == "Arduino.h":
            content = content.replace(b'#include <thread>', b'#include "freertos/task.h"')
            old = b'std::this_thread::sleep_for(std::chrono::milliseconds(ms));'
            if old not in content:
                raise RuntimeError("Simulator Arduino delay adapter changed; review the browser overlay")
            content = content.replace(old, b'vTaskDelay(pdMS_TO_TICKS(ms));')
            content = content.replace(b'std::this_thread::yield();', b'vTaskDelay(0);')
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or destination.read_bytes() != content:
            destination.write_bytes(content)
    return target


def translated_command(entry, compiler, obj, simulator, original_simulator):
    args = entry.get("arguments") or shlex.split(entry["command"])
    result = [compiler, f"-I{HERE / 'stubs'}", f"-I{simulator}"]
    skip = False
    for arg in args[1:]:
        arg = arg.replace(str(original_simulator), str(simulator))
        arg = arg.replace(str(original_simulator.relative_to(ROOT)), str(simulator))
        if skip:
            skip = False
            continue
        if arg in ("-o", "-arch", "-isysroot"):
            skip = True
        elif arg == "-c" or arg.endswith((".cpp", ".c", ".cc")):
            continue
        elif arg.startswith(("-l", "-L", "-O", "-g")) or "SDL2" in arg:
            continue
        elif compiler.endswith("emcc") and arg.startswith("-std="):
            continue
        else:
            result.append(arg)
    result += ["-funsigned-char", "-fno-exceptions", "-Dmemcpy_P=memcpy", "-include", "sys/time.h",
               "-Oz", "-Wno-unused-command-line-argument",
               "-MMD", "-MF", str(obj) + ".d", "-c", entry["file"], "-o", str(obj)]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", choices=("x4", "x3", "x4pro"), default="x4")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    empp = shutil.which("em++")
    if not empp or not shutil.which("ninja"):
        parser.error("Activate Emscripten 6.0.6 and install ninja. See tools/wasm/README.md.")
    version = run([empp, "--version"], capture_output=True, text=True).stdout
    if f" {EMSDK_VERSION} " not in version:
        parser.error(f"Use the pinned Emscripten {EMSDK_VERSION}; got {version.splitlines()[0]}")
    env = f"preview-{args.device}"
    run(["pio", "run", "-c", "platformio.preview.ini", "-e", env, "-t", "compiledb"])
    entries = json.loads((ROOT / "compile_commands.json").read_text())
    objects = ROOT / ".cache/wasm" / args.device
    output = ROOT / "build/preview"
    target = output / args.device
    objects.mkdir(parents=True, exist_ok=True)
    target.mkdir(parents=True, exist_ok=True)
    seed = objects / "sdcard"
    seed_card(seed)
    original_simulator = ROOT / ".pio/libdeps" / env / "simulator/src"
    simulator = browser_simulator(original_simulator, objects / "simulator")
    # Always regenerate the source list; Ninja tracks command and header changes.
    entries = [entry for entry in entries if Path(entry["file"]).name not in
               {"simulator_main.cpp", "HttpDownloader.cpp", "CrossPointWebServer.cpp"}]
    template = next(entry for entry in entries if Path(entry["file"]).resolve() == ROOT / "src/main.cpp")
    for source in sorted((HERE / "src").glob("*.cpp")):
        extra = dict(template)
        extra["file"] = str(source)
        entries.append(extra)
    rules = [f"builddir = {ninja_path(objects)}", "rule compile", "  command = $command", "  description = CXX $in",
             "  depfile = $out.d", "  deps = gcc", "rule link", "  command = $command", "  description = LINK $out"]
    obj_paths = []
    for entry in entries:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        source = source.resolve()
        if source.is_relative_to(original_simulator):
            source = simulator / source.relative_to(original_simulator)
        entry["file"] = str(source)
        obj = objects / (hashlib.sha256(str(source).encode()).hexdigest()[:16] + ".o")
        compiler = str(Path(empp).with_name("emcc")) if source.suffix == ".c" else empp
        command = translated_command(entry, compiler, obj, simulator, original_simulator)
        rules += [f"build {ninja_path(obj)}: compile {ninja_path(source)}",
                  "  command = " + shlex.join(command).replace("$", "$$")]
        obj_paths.append(obj)
    exports = ["_main", "_preview_start", "_preview_step", "_preview_stop", "_preview_take_frame", "_preview_width", "_preview_height",
               "_preview_rotation", "_preview_button", "_preview_release_buttons", "_preview_touch", "_preview_mouse_swipe",
               "_preview_active_book", "_preview_active_font", "_preview_path_hash"]
    module = target / "firmware.js"
    link = [empp, *map(str, obj_paths), "-o", str(module), "-Oz",
            "-sASYNCIFY=1", "-sSTACK_SIZE=1MB",
            "-sINITIAL_MEMORY=67108864", "-sMAXIMUM_MEMORY=268435456", "-sALLOW_MEMORY_GROWTH=1", "-sEXIT_RUNTIME=0",
            "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createCrosspoint",
            "-sENVIRONMENT=web", "-sEXPORTED_RUNTIME_METHODS=FS,HEAPU32,UTF8ToString,ccall",
            "-sINCOMING_MODULE_JS_API=wasmBinary,locateFile,print,printErr,onAbort,preRun",
            "-sEXPORTED_FUNCTIONS=" + ",".join(exports), "-sASSERTIONS=1",
            "--preload-file", f"{seed}@/fs_"]
    rules += [f"build {ninja_path(module)}: link " + " ".join(map(ninja_path, obj_paths)) +
              " | " + " ".join(ninja_path(p) for p in sorted(seed.rglob("*")) if p.is_file()),
              "  command = " + shlex.join(link).replace("$", "$$")]
    build_file = objects / "build.ninja"
    build_file.write_text("\n".join(rules) + "\n")
    run(["ninja", "-f", str(build_file), "-j", str(args.jobs)])
    for source in (HERE / "web").iterdir():
        if source.is_file():
            shutil.copyfile(source, output / source.name)
    shutil.copyfile(HERE / "LICENSE.crossplay", output / "LICENSE.crossplay.txt")
    shutil.copyfile(ROOT / "LICENSE", output / "LICENSE.txt")
    sha = run(["git", "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = bool(run(["git", "status", "--porcelain"], capture_output=True, text=True).stdout.strip())
    manifest = {"device": args.device, "sha": sha, "dirty": dirty, "emscripten": EMSDK_VERSION,
                "simulator": "097f44e08492d9dde40d45c1bce189fd1e782e0e", "runtime": "cooperative-asyncify"}
    (target / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    package(output, args.device)
    print(f"Preview built: {output}\nRun: python3 tools/wasm/serve.py")


if __name__ == "__main__":
    main()
