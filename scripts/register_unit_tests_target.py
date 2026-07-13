"""
PlatformIO post-load script: register a `unit-tests` custom target so
`pio run -t unit-tests` builds and runs the host gtest suites under test/.

The target shells out to CMake/CTest; the gtest framework is fetched and
the suites are built outside the PlatformIO/ESP-IDF toolchain (this is a
host build, not a firmware build).
"""

import os

Import("env")  # noqa: F821  -- provided by PlatformIO at script load

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
BUILD_DIR = os.path.join(PROJECT_DIR, "build", "test")
TEST_SRC_DIR = os.path.join(PROJECT_DIR, "test")

env.AddCustomTarget(  # noqa: F821
    name="unit-tests",
    dependencies=None,
    actions=[
        f'cmake -S "{TEST_SRC_DIR}" -B "{BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release',
        f'cmake --build "{BUILD_DIR}"',
        f'ctest --test-dir "{BUILD_DIR}" --output-on-failure -j',
    ],
    title="Host unit tests",
    description="Build and run gtest suites in test/ via CMake/CTest",
)
