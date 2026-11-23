#!/usr/bin/env python3
"""Exercise the portable and S3-wrapper paths without Arduino or a device."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parents[1] / "lib/ImageOps"
SRC = LIB / "src"
INCLUDE = LIB / "include"
CXX = [os.environ.get("CXX", "c++"), *shlex.split(os.environ.get("CXX_COMPILER_ARG1", ""))]
with tempfile.TemporaryDirectory(prefix="crosspoint-image-ops-") as directory:
    for mode, enabled, defines in (
        ("portable", False, []),
        ("c3", False, ["-DCONFIG_IDF_TARGET_ESP32C3=1", "-DCONFIG_IDF_TARGET_ESP32S3=0"]),
        ("c3-enabled", True, ["-DCONFIG_IDF_TARGET_ESP32C3=1", "-DCONFIG_IDF_TARGET_ESP32S3=0",
                        "-DCROSSPOINT_ACCELERATED_IMAGES=1"]),
        ("s3-model", True, ["-DCONFIG_IDF_TARGET_ESP32S3=1"]),
        ("s3-disabled", False, ["-DCONFIG_IDF_TARGET_ESP32S3=1", "-DCROSSPOINT_ACCELERATED_IMAGES=0"]),
    ):
        subprocess.run(
            [*CXX, "-E", "-P", "-x", "c++", *defines, "-I" + str(INCLUDE), "-"],
            input=('#include "ImageOpsConfig.h"\n'
                   f'#if CROSSPOINT_ACCELERATED_IMAGES != {int(enabled)} || '
                   f'USE_SIMD_S3_PIE != {int(mode == "s3-model")}\n'
                   '#error "Unexpected image kernel configuration"\n#endif\n'),
            text=True, check=True,
        )
        sources = [SRC / "BayerDither.cpp", SRC / "TilePlanes.cpp", SRC / "Rgb24.cpp",
                   SRC / "Indexed.cpp"]
        if enabled:
            sources.extend(HERE / name for name in ("test_image_ops.cpp", "test_tile_planes.cpp",
                                                    "test_rgb24.cpp", "test_indexed.cpp"))
            if mode == "s3-model":
                sources.extend(HERE / name for name in ("image_ops_model.cpp", "tile_planes_model.cpp",
                                                        "rgb24_model.cpp", "indexed_model.cpp"))
            binary = Path(directory) / mode
            subprocess.run(
                [*CXX, "-std=c++17", "-Os", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", *defines, "-I" + str(INCLUDE),
                 *map(str, sources), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
        else:
            for source in sources:
                code = subprocess.check_output(
                    [*CXX, "-E", "-P", *defines, "-I" + str(INCLUDE), str(source)], text=True,
                )
                if code.strip():
                    raise RuntimeError(f"{mode}: {source.name} must be empty before compilation")
        if mode != "s3-model":
            for source in ("BayerSpanS3.S", "LevelTilePlanesS3.S", "PackedTilePlanesS3.S", "Rgb24GrayS3.S",
                           "IndexUnpackS3.S"):
                assembly = subprocess.check_output(
                    [*CXX, "-E", "-P", "-x", "assembler-with-cpp", *defines, "-I" + str(INCLUDE),
                     str(SRC / "arch/esp32s3" / source)], text=True,
                )
                if assembly.strip():
                    raise RuntimeError(f"{mode}: {source} must be empty before assembly")
        print(f"PASS: {mode}", flush=True)
