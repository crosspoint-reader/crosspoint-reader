#!/usr/bin/env python3
"""Embed bundled .cpapp archives into flash-resident headers for app_store builds."""

from __future__ import annotations

import json
import re
import sys
import zipfile
from pathlib import Path

def project_dir_from_env(env_name: str | None = None) -> Path:
    try:
        Import("env")  # noqa: F821  # type: ignore[name-defined]
        return Path(env["PROJECT_DIR"])  # noqa: F821  # type: ignore[name-defined]
    except NameError:
        return Path(__file__).resolve().parents[1]


def paths_for_project(project_dir: Path) -> tuple[Path, Path, Path, Path, Path]:
    return (
        project_dir / "data" / "bundled_apps.json",
        project_dir / "apiserver" / "data" / "manifest.json",
        project_dir / "apiserver" / "public" / "v1" / "bundles",
        project_dir / "lib" / "AppStore" / "BundledAppsData.generated.h",
        project_dir / "lib" / "AppStore" / "AppStoreManifestBuiltin.generated.h",
    )

HELLO_BUILTIN_JSON = """{
  "version": 1,
  "apps": [
    {
      "id": "hello",
      "name": "Hello App",
      "version": "1.0.0",
      "description": "Smoke-test sample for the Lua runtime",
      "min_api_version": "1.0"
    }
  ]
}"""


def c_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def sanitize_symbol(value: str) -> str:
    symbol = re.sub(r"[^a-zA-Z0-9_]", "_", value)
    if not symbol or symbol[0].isdigit():
        symbol = f"app_{symbol}"
    return symbol


def load_catalog_by_id(apiserver_manifest: Path) -> dict[str, dict]:
    if not apiserver_manifest.is_file():
        return {}
    data = json.loads(apiserver_manifest.read_text(encoding="utf-8"))
    return {app["id"]: app for app in data.get("apps", []) if "id" in app}


def read_bundle_manifest(archive_path: Path) -> dict:
    with zipfile.ZipFile(archive_path, "r") as zf:
        with zf.open("manifest.json") as manifest_file:
            return json.load(manifest_file)


def write_stub_headers(project_dir: Path) -> None:
    _, _, _, out_bundled, out_builtin = paths_for_project(project_dir)
    bundled = """#pragma once

#include <cstddef>
#include <cstdint>

struct BundledAppBlob {
  const char* id;
  const char* name;
  const char* version;
  const uint8_t* data;
  size_t size;
};

namespace BundledApps {

inline constexpr BundledAppBlob kApps[] = {};
inline constexpr size_t kAppCount = 0;

}  // namespace BundledApps
"""
    builtin = f"""#pragma once

namespace AppStoreManifestData {{

inline constexpr char kBuiltinJson[] = R"cpmanifest({HELLO_BUILTIN_JSON})cpmanifest";

}}  // namespace AppStoreManifestData
"""
    out_bundled.parent.mkdir(parents=True, exist_ok=True)
    out_bundled.write_text(bundled, encoding="utf-8")
    out_builtin.write_text(builtin, encoding="utf-8")
    print("embed_bundled_apps: wrote stub headers (no bundled apps)")


def format_byte_array(data: bytes, indent: str = "  ") -> str:
    lines: list[str] = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        hex_bytes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"{indent}{hex_bytes},")
    return "\n".join(lines)


def write_full_headers(project_dir: Path) -> None:
    manifest_path, apiserver_manifest, bundles_dir, out_bundled, out_builtin = paths_for_project(project_dir)
    if not manifest_path.is_file():
        print(f"embed_bundled_apps: missing {manifest_path}", file=sys.stderr)
        sys.exit(1)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    apps = manifest.get("apps", [])
    catalog = load_catalog_by_id(apiserver_manifest)

    blob_sections: list[str] = []
    table_rows: list[str] = []
    builtin_apps: list[dict] = []

    for entry in apps:
        app_id = entry["id"]
        archive = entry["archive"]
        archive_path = bundles_dir / archive
        if not archive_path.is_file():
            print(
                f"embed_bundled_apps: missing bundle {archive_path}\n"
                "Run: cd apiserver && python3 scripts/build.py",
                file=sys.stderr,
            )
            sys.exit(1)

        bundle_bytes = archive_path.read_bytes()
        bundle_manifest = read_bundle_manifest(archive_path)
        catalog_entry = catalog.get(app_id, {})

        name = catalog_entry.get("name") or bundle_manifest.get("name") or app_id
        version = catalog_entry.get("version") or bundle_manifest.get("version") or "0.0.0"
        description = catalog_entry.get("description") or bundle_manifest.get("description") or ""
        min_api = catalog_entry.get("min_api_version") or bundle_manifest.get("min_api_version") or "1.0"

        symbol = sanitize_symbol(app_id)
        blob_sections.append(
            f"alignas(4) const uint8_t kBlob_{symbol}[] = {{\n"
            f"{format_byte_array(bundle_bytes)}\n"
            "};"
        )
        table_rows.append(
            f'  {{"{c_escape(app_id)}", "{c_escape(name)}", "{c_escape(version)}", '
            f"kBlob_{symbol}, {len(bundle_bytes)}u}},"
        )
        builtin_apps.append(
            {
                "id": app_id,
                "name": name,
                "version": version,
                "description": description,
                "min_api_version": min_api,
            }
        )

    bundled_header = f"""#pragma once

#include <cstddef>
#include <cstdint>

struct BundledAppBlob {{
  const char* id;
  const char* name;
  const char* version;
  const uint8_t* data;
  size_t size;
}};

namespace BundledApps {{

{chr(10).join(blob_sections)}

inline const BundledAppBlob kApps[] = {{
{chr(10).join(table_rows)}
}};

inline const size_t kAppCount = sizeof(kApps) / sizeof(kApps[0]);

}}  // namespace BundledApps
"""

    builtin_json = json.dumps({"version": 1, "apps": builtin_apps}, indent=2)
    builtin_header = f"""#pragma once

namespace AppStoreManifestData {{

inline constexpr char kBuiltinJson[] = R"cpmanifest({builtin_json})cpmanifest";

}}  // namespace AppStoreManifestData
"""

    out_bundled.parent.mkdir(parents=True, exist_ok=True)
    out_bundled.write_text(bundled_header, encoding="utf-8")
    out_builtin.write_text(builtin_header, encoding="utf-8")

    total_bytes = sum((bundles_dir / entry["archive"]).stat().st_size for entry in apps)
    print(f"embed_bundled_apps: embedded {len(apps)} apps ({total_bytes} bytes)")


def should_embed_full(env_name: str | None) -> bool:
    if env_name == "app_store":
        return True
    if "--full" in sys.argv:
        return True
    return False


def main(env_name: str | None = None, project_dir: Path | None = None) -> None:
    root = project_dir if project_dir is not None else project_dir_from_env()
    if should_embed_full(env_name):
        write_full_headers(root)
    else:
        write_stub_headers(root)


if __name__ == "__main__":
    main()
else:
    try:
        Import("env")  # noqa: F821  # type: ignore[name-defined]
        main(env["PIOENV"], Path(env["PROJECT_DIR"]))  # noqa: F821  # type: ignore[name-defined]
    except NameError:
        main()
