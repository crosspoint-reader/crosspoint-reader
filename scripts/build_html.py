import configparser
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

import zopfli.gzip  # type: ignore[import-not-found]

SRC_DIR = "src"

PLACEHOLDERS = {}

# Assets the firmware keeps in flash. Everything else is served from the SD card:
# it goes into one bundle, web-<hash>.bin, which the firmware pins by SHA-256 and
# installs into /.crosspoint/web (src/network/WebAssets.h). The setup page stays,
# because it is what installs the bundle.
FLASH_ASSETS = {"WebSetupPage.html"}

# Files made outside src/network/html that pages load from /js/<name>: the Font Manager's converter,
# FreeType built to WebAssembly with its JS around it (tools/ftwasm; rebuild with its build.sh), and
# the licences that must ship with it. Bundled as they are, gzipped, never minified. Paths are
# relative to the project; the name is the one under /js/.
EXTERNAL_ASSETS = [
    ("ftwasm.wasm", "tools/ftwasm/dist/ftwasm.wasm", "application/wasm"),
    ("ftwasm.mjs", "tools/ftwasm/dist/ftwasm.mjs", "application/javascript"),
    ("cpfont-raster.mjs", "tools/ftwasm/js/cpfont-raster.mjs", "application/javascript"),
    ("cpfont-writer.mjs", "tools/ftwasm/js/cpfont-writer.mjs", "application/javascript"),
    ("otf-tables.mjs", "tools/ftwasm/js/otf-tables.mjs", "application/javascript"),
    ("pyset-order.mjs", "tools/ftwasm/js/pyset-order.mjs", "application/javascript"),
    ("kern-reduce.mjs", "tools/ftwasm/js/kern-reduce.mjs", "application/javascript"),
    ("FreeType-FTL.txt", "tools/ftwasm/dist/licenses/FreeType-FTL.txt", "text/plain"),
    ("HarfBuzz-COPYING.txt", "tools/ftwasm/dist/licenses/HarfBuzz-COPYING.txt", "text/plain"),
]
BUNDLE_HEADER = os.path.join(SRC_DIR, "network", "html", "WebBundle.generated.h")

# <!--#include file="shared/Foo.js.inc" --> directives, resolved relative to the
# including file's directory. Included fragments use the .inc suffix so the
# asset walk below skips them — they are only ever inlined, never compiled to a
# header of their own.
INCLUDE_RE = re.compile(r'<!--\s*#include\s+file="([^"]+)"\s*-->')
MAX_INCLUDE_DEPTH = 8

def warn(msg: str) -> None:
    print(f"WARNING [build_html.py]: {msg}", file=sys.stderr)


def resolve_includes(html: str, base_dir: str, included: list) -> str:
    """Inline #include directives, appending each resolved path to `included`.

    Must run BEFORE minify_html, which strips every HTML comment. Repeats so an
    included fragment may itself include others; the caller stats `included` so
    editing a fragment invalidates the generated header of every page using it.

    Ported from crosspoint-reader PR #2734 by Justin Mitchell (@itsthisjustin) -
    the directive syntax, the substitution loop and its depth bound are his.
    Added here: recording resolved paths, because this fork's asset pipeline
    caches on mtime and would otherwise leave a page stale when only the
    fragment changed; and failing the build loudly on a missing include.
    """

    def repl(match):
        inc_path = os.path.normpath(os.path.join(base_dir, match.group(1)))
        included.append(inc_path)
        try:
            with open(inc_path, "r", encoding="utf-8") as f:
                return f.read()
        except OSError as e:
            raise SystemExit(
                f'ERROR [build_html.py]: cannot resolve #include "{match.group(1)}"'
                f" from {base_dir}: {e}"
            )

    for _ in range(MAX_INCLUDE_DEPTH):
        html, count = INCLUDE_RE.subn(repl, html)
        if count == 0:
            return html
    warn(f"#include nesting exceeded {MAX_INCLUDE_DEPTH} levels (circular include?)")
    return html


def get_base_version(project_dir: str) -> (str, str):
    ini_path = os.path.join(project_dir, "platformio.ini")
    if not os.path.isfile(ini_path):
        warn(f"platformio.ini not found at {ini_path}; using 0.0.0")
        return "CrossPoint Reader", "0.0.0"
    config = configparser.ConfigParser()
    config.read(ini_path)
    base_name = config.get("crosspoint", "name", fallback="CrossPoint Reader")
    base_name = base_name.strip('"')
    if not config.has_option("crosspoint", "version"):
        warn("No [crosspoint] section or version in platformio.ini; using 0.0.0")
        return base_name, "0.0.0"
    return base_name, config.get("crosspoint", "version")


def get_git_branch(project_dir: str) -> str:
    try:
        branch = subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            text=True,
            stderr=subprocess.PIPE,
            cwd=project_dir,
        ).strip()
        if branch == "HEAD":
            branch = subprocess.check_output(
                ["git", "rev-parse", "--short", "HEAD"],
                text=True,
                stderr=subprocess.PIPE,
                cwd=project_dir,
            ).strip()
        return "".join(c for c in branch if c not in '"\\')
    except FileNotFoundError:
        warn('git not found on PATH; branch suffix will be "unknown"')
        return "unknown"
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): {e.stderr.strip()}; branch suffix will be "unknown"'
        )
        return "unknown"
    except Exception as e:
        warn(
            f'Unexpected error reading git branch: {e}; branch suffix will be "unknown"'
        )
        return "unknown"


def get_version_string(project_dir: str) -> (str, str):
    env_name = os.environ.get("CROSSPOINT_NAME")
    env_version = os.environ.get("CROSSPOINT_VERSION")
    if env_version:
        return env_name.strip('"'), env_version.strip('"')
    base_name, base_version = get_base_version(project_dir)
    pioenv = os.environ.get("PIOENV", "default")
    if pioenv == "default":
        branch = get_git_branch(project_dir)
        return base_name, f"{base_version}-dev+{branch}"
    return base_name, base_version


def replace_placeholders(html: str, replacements: dict) -> str:
    for placeholder, replacement in replacements.items():
        html = html.replace(placeholder, replacement)
    return html


def strip_js_comments(js: str) -> str:
    """Remove JS comments while preserving string literals and URLs."""
    result = []
    i = 0
    length = len(js)
    while i < length:
        # String literals — pass through unchanged
        if js[i] in ('"', "'", "`"):
            quote = js[i]
            result.append(js[i])
            i += 1
            while i < length:
                if js[i] == "\\" and i + 1 < length:
                    result.append(js[i : i + 2])
                    i += 2
                elif js[i] == quote:
                    result.append(js[i])
                    i += 1
                    break
                else:
                    result.append(js[i])
                    i += 1
        # Block comment /* ... */
        elif js[i] == "/" and i + 1 < length and js[i + 1] == "*":
            end = js.find("*/", i + 2)
            i = end + 2 if end != -1 else length
        # Line comment // ...
        elif js[i] == "/" and i + 1 < length and js[i + 1] == "/":
            end = js.find("\n", i)
            if end == -1:
                i = length
            else:
                # Keep the newline to preserve line structure
                result.append("\n")
                i = end + 1
        # Regex literal — pass through unchanged
        # Heuristic: / after = ( , ; ! & | ? : [ { } ~ ^ or line start
        elif js[i] == "/" and i > 0:
            # Look back for operator context (skip whitespace)
            j = i - 1
            while j >= 0 and js[j] in " \t":
                j -= 1
            if j >= 0 and js[j] in "=(!,;:&|?[{}>~^+-*%":
                result.append(js[i])
                i += 1
                while i < length:
                    if js[i] == "\\" and i + 1 < length:
                        result.append(js[i : i + 2])
                        i += 2
                    elif js[i] == "/":
                        result.append(js[i])
                        i += 1
                        # Regex flags
                        while i < length and js[i].isalpha():
                            result.append(js[i])
                            i += 1
                        break
                    elif js[i] == "[":
                        # Character class — / doesn't end regex inside []
                        result.append(js[i])
                        i += 1
                        while i < length and js[i] != "]":
                            if js[i] == "\\" and i + 1 < length:
                                result.append(js[i : i + 2])
                                i += 2
                            else:
                                result.append(js[i])
                                i += 1
                    else:
                        result.append(js[i])
                        i += 1
            else:
                result.append(js[i])
                i += 1
        else:
            result.append(js[i])
            i += 1
    return "".join(result)


_TERSER = "unresolved"  # cached: None once we know it is unavailable


def find_terser(project_dir: str):
    """Locate terser, or None. Resolved once per build.

    Invoked through `node <entry>` rather than the .bin shim so the same code
    path works on Windows and POSIX.
    """
    global _TERSER
    if _TERSER != "unresolved":
        return _TERSER

    entry = os.path.join(project_dir, "node_modules", "terser", "bin", "terser")
    if os.path.isfile(entry) and shutil.which("node"):
        _TERSER = ["node", entry]
    else:
        _TERSER = None
        warn(
            "terser not found - inline JS keeps its original identifiers, so the "
            "generated pages are roughly 17% larger than a release build. "
            "Run `npm install` in the project root to match CI."
        )
    return _TERSER


def minify_js(js: str, project_dir: str) -> str:
    """Compress and mangle one inline <script> body.

    Mangles local names only: `toplevel` is deliberately NOT set, because the
    pages call top-level functions from inline HTML attributes
    (onclick="validateFile()") which terser cannot see and would rename into
    oblivion. Same reason unused top-level functions must not be dropped.

    Falls back to the caller's whitespace/comment stripping if terser is absent
    or rejects the input - a bigger page is always preferable to a broken one.
    """
    terser = find_terser(project_dir)
    if not terser or not js.strip():
        return strip_js_comments(js)

    try:
        result = subprocess.run(
            terser + ["--compress", "--mangle"],
            input=js,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
    except OSError as e:
        warn(f"could not run terser ({e}); falling back to comment stripping")
        return strip_js_comments(js)

    if result.returncode != 0 or not result.stdout.strip():
        warn(f"terser failed, falling back to comment stripping: {(result.stderr or '').strip()[:200]}")
        return strip_js_comments(js)
    return result.stdout


def minify_html(html: str, project_dir: str = "") -> str:
    # Tags where whitespace should be preserved
    preserve_tags = ["pre", "code", "textarea"]
    script_style_tags = ["script", "style"]
    preserve_regex = "|".join(preserve_tags)
    script_style_regex = "|".join(script_style_tags)

    # Protect preserve blocks (pre/code/textarea) with placeholders
    preserve_blocks = []

    def preserve(match):
        preserve_blocks.append(match.group(0))
        return f"__PRESERVE_BLOCK_{len(preserve_blocks) - 1}__"

    html = re.sub(
        rf"<({preserve_regex})[\s\S]*?</\1>", preserve, html, flags=re.IGNORECASE
    )

    # Strip JS/CSS comments inside <script>/<style> blocks, then protect them
    def strip_and_preserve(match):
        tag = match.group(1).lower()
        full = match.group(0)
        # Extract content between opening and closing tags
        open_end = full.index(">") + 1
        close_start = full.rindex("<")
        opening = full[:open_end]
        content = full[open_end:close_start]
        closing = full[close_start:]
        if tag == "script":
            content = minify_js(content, project_dir)
        elif tag == "style":
            # Remove CSS comments
            content = re.sub(r"/\*.*?\*/", "", content, flags=re.DOTALL)
        preserve_blocks.append(f"{opening}{content}{closing}")
        return f"__PRESERVE_BLOCK_{len(preserve_blocks) - 1}__"

    html = re.sub(
        rf"<({script_style_regex})[\s\S]*?</\1>",
        strip_and_preserve,
        html,
        flags=re.IGNORECASE,
    )

    # Remove HTML comments
    html = re.sub(r"<!--.*?-->", "", html, flags=re.DOTALL)

    # Collapse all whitespace between tags
    html = re.sub(r">\s+<", "><", html)

    # Collapse multiple spaces inside tags
    html = re.sub(r"\s+", " ", html)

    # Restore preserved blocks
    for i, block in enumerate(preserve_blocks):
        html = html.replace(f"__PRESERVE_BLOCK_{i}__", block)

    return html.strip()


def sanitize_identifier(name: str) -> str:
    """Sanitize a filename to create a valid C identifier.

    C identifiers must:
    - Start with a letter or underscore
    - Contain only letters, digits, and underscores
    """
    # Replace non-alphanumeric characters (including hyphens) with underscores
    sanitized = re.sub(r"[^a-zA-Z0-9_]", "_", name)
    # Prefix with underscore if starts with a digit
    if sanitized and sanitized[0].isdigit():
        sanitized = f"_{sanitized}"
    return sanitized


def get_project_dir() -> str:
    try:
        Import("env")  # type: ignore[name-defined]
        return env["PROJECT_DIR"]
    except NameError:
        if "__file__" in globals():
            script_dir = os.path.dirname(os.path.abspath(__file__))
        elif sys.argv and sys.argv[0]:
            script_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
        else:
            script_dir = os.getcwd()
        return os.path.dirname(script_dir)


def get_scons_env():
    """The PlatformIO build environment, or None when run outside a build."""
    try:
        Import("env")  # type: ignore[name-defined]
        return env  # type: ignore[name-defined]
    except NameError:
        return None


def get_release_tag(pioenv: str, base_version: str) -> str:
    """The GitHub release this build is published under, or "" when it is not.

    The firmware downloads its web bundle from that release (src/network/WebAssets.h).
    The release envs stamp the plain base version, which is the release tag; the RC
    envs are built for the tag CI created for them. Dev, slim, bench and the board dev
    envs are never published, so the setup page asks for an upload instead.
    """
    if pioenv.endswith("gh_release_rc"):
        return os.environ.get("CROSSPOINT_RC_VERSION", "")
    if pioenv.endswith("gh_release"):
        return base_version
    return ""


def content_type(file: str) -> str:
    return "text/html" if file.endswith(".html") else "application/javascript"


def write_if_changed(path: str, text: str) -> bool:
    """Write `text` unless the file already holds it, so an unchanged table does
    not recompile the sources that include it."""
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == text:
                return False
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return True


def render_bundle_header(entries: list, digest: bytes, asset_name: str, total: int) -> str:
    lines = [
        "// THIS FILE IS AUTOGENERATED, DO NOT EDIT MANUALLY",
        "",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "// The web UI bundle this firmware serves from the SD card. See src/network/WebAssets.h.",
        "namespace web_bundle {",
        "",
        "struct Entry {",
        "  const char* file;  // name under /.crosspoint/web, and its order in the bundle",
        "  const char* contentType;",
        "  uint32_t size;  // gzip bytes",
        "};",
        "",
        "enum Index : uint8_t {",
    ]
    lines += [f"  {ident} = {i}," for i, (_, ident, _, _) in enumerate(entries)]
    lines += ["};", "", "inline constexpr Entry ENTRIES[] = {"]
    lines += [f'    {{"{name}", "{ctype}", {len(data)}}},' for name, _, ctype, data in entries]
    lines += [
        "};",
        "inline constexpr size_t ENTRY_COUNT = sizeof(ENTRIES) / sizeof(ENTRIES[0]);",
        f"inline constexpr uint32_t TOTAL_SIZE = {total};",
        f'inline constexpr char ASSET_NAME[] = "{asset_name}";',
        "inline constexpr uint8_t DIGEST[32] = {  // SHA-256 of the whole bundle",
    ]
    lines += ["    " + ", ".join(f"0x{b:02x}" for b in digest[i : i + 16]) + "," for i in (0, 16)]
    lines += ["};", "", "}  // namespace web_bundle", ""]
    return "\n".join(lines)


def write_bundle(entries: list, out_dir: str) -> None:
    """Join the bundle's files into web-<hash>.bin and describe it to the firmware.

    The order is sorted, not walk order, so every machine and every CI leg builds
    byte-identical bundles: the firmware pins the bundle's hash, and one release
    asset serves every board.
    """
    entries.sort(key=lambda e: e[0])
    names = [e[0] for e in entries]
    if len(set(names)) != len(names):
        raise SystemExit(f"ERROR [build_html.py]: two web assets share a file name: {names}")

    blob = b"".join(e[3] for e in entries)
    digest = hashlib.sha256(blob).digest()
    asset_name = f"web-{digest.hex()[:12]}.bin"
    header = render_bundle_header(entries, digest, asset_name, len(blob))
    if write_if_changed(BUNDLE_HEADER, header):
        print(f"Generated: {BUNDLE_HEADER}")

    os.makedirs(out_dir, exist_ok=True)
    for old in glob.glob(os.path.join(out_dir, "web-*.bin")):
        if os.path.basename(old) != asset_name:
            os.remove(old)
    with open(os.path.join(out_dir, asset_name), "wb") as f:
        f.write(blob)
    print(f"Web bundle: {asset_name} ({len(blob)} bytes, {len(entries)} files) in {out_dir}")


project_dir = get_project_dir()
scons_env = get_scons_env()
name_string, version_string = get_version_string(project_dir)
print (f"Building HTML/JS assets for {name_string} version {version_string}")
PLACEHOLDERS["%%CROSSPOINT%%"] = name_string
PLACEHOLDERS["%%VERSION%%"] = version_string
ini_path = os.path.join(project_dir, "platformio.ini")
ini_time = os.path.getmtime(ini_path) if os.path.exists(ini_path) else 0
# Resolved once, before the walk, so the warning prints at most one time and
# so its mtime can take part in the staleness check below.
_terser_cmd = find_terser(project_dir)
terser_time = os.path.getmtime(_terser_cmd[1]) if _terser_cmd else 0
bundle_entries = []  # (file name on the card, C identifier, content type, gzip bytes)
for root, _, files in os.walk(SRC_DIR):
    for file in files:
        if file.endswith(".html") or file.endswith(".js"):
            file_path = os.path.join(root, file)
            with open(file_path, "r", encoding="utf-8") as f:
                content = f.read()

            # Resolve includes first, so a fragment gets placeholder substitution
            # and comment stripping like any inline script, and so the freshness
            # check below knows every file the page is built from.
            included = []
            if file.endswith(".html"):
                content = resolve_includes(content, root, included)

            # Create valid C identifier from filename
            # Use appropriate suffix based on file type
            suffix = "Html" if file.endswith(".html") else "Js"
            base_name = sanitize_identifier(f"{os.path.splitext(file)[0]}{suffix}")
            # A flash asset compiles to a header; a bundle asset is cached as its
            # gzip file, which the bundle is assembled from on every build.
            in_flash = file in FLASH_ASSETS
            out_path = os.path.join(root, f"{base_name}.generated.h" if in_flash else f"{base_name}.generated.gz")
            # A header left from when this asset was still in flash would only mislead.
            old_header = os.path.join(root, f"{base_name}.generated.h")
            if not in_flash and os.path.exists(old_header):
                os.remove(old_header)

            # Editing an included fragment must invalidate the output too, so
            # the freshness bar is the newest of the page, its includes and the
            # ini. Terser's own mtime joins them: installing it after a build
            # changes the output, and without this the cached outputs would stay
            # at their larger un-mangled size.
            newest_src = max(
                [os.path.getmtime(file_path), ini_time, terser_time]
                + [os.path.getmtime(p) for p in included if os.path.exists(p)]
            )
            if os.path.exists(out_path) and os.path.getmtime(out_path) >= newest_src:
                print(f"Unchanged: {out_path}")
                if not in_flash:
                    with open(out_path, "rb") as g:
                        bundle_entries.append((f"{file}.gz", base_name, content_type(file), g.read()))
                continue

            # Replace build-time placeholders only in HTML files
            if file.endswith(".html"):
                content = replace_placeholders(content, PLACEHOLDERS)
                processed = minify_html(content, project_dir)
            else:
                processed = content

            # Zopfli emits a standards-compatible gzip stream with maximal compression.
            # IMPORTANT: we don't use brotli because Firefox doesn't support brotli with insecured context (only supported on HTTPS)
            compressed = zopfli.gzip.compress(processed.encode("utf-8"))

            if in_flash:
                with open(out_path, "w", encoding="utf-8") as h:
                    h.write("// THIS FILE IS AUTOGENERATED, DO NOT EDIT MANUALLY\n\n")
                    h.write("#pragma once\n")
                    h.write("#include <cstddef>\n\n")
                    h.write(f"constexpr char {base_name}[] PROGMEM = {{\n")
                    for i in range(0, len(compressed), 16):
                        chunk = compressed[i : i + 16]
                        hex_values = ", ".join(f"0x{b:02x}" for b in chunk)
                        h.write(f"  {hex_values},\n")
                    h.write("};\n\n")
                    h.write(f"constexpr size_t {base_name}CompressedSize = {len(compressed)};\n")
                    h.write(f"constexpr size_t {base_name}OriginalSize = {len(processed)};\n")
            else:
                with open(out_path, "wb") as g:
                    g.write(compressed)
                bundle_entries.append((f"{file}.gz", base_name, content_type(file), compressed))
            print(f"Generated: {out_path}")
            print(f"  Original: {len(content)} bytes")
            print(
                f"  Minified: {len(processed)} bytes ({100 * len(processed) / len(content):.1f}%)"
            )
            print(
                f"  Compressed: {len(compressed)} bytes ({100 * len(compressed) / len(content):.1f}%)"
            )

# The external assets, gzipped once and kept in .pio/web-cache until their source changes.
cache_dir = os.path.join(project_dir, ".pio", "web-cache")
os.makedirs(cache_dir, exist_ok=True)
for name, rel_source, ctype in EXTERNAL_ASSETS:
    source = os.path.join(project_dir, rel_source)
    if not os.path.isfile(source):
        raise SystemExit(f"ERROR [build_html.py]: missing {rel_source}, which the web bundle needs")
    stem, ext = os.path.splitext(name)
    ident = sanitize_identifier(f"{stem}{ext[1:].capitalize()}")
    cached = os.path.join(cache_dir, f"{name}.gz")
    if os.path.exists(cached) and os.path.getmtime(cached) >= os.path.getmtime(source):
        with open(cached, "rb") as g:
            compressed = g.read()
    else:
        with open(source, "rb") as f:
            compressed = zopfli.gzip.compress(f.read())
        with open(cached, "wb") as g:
            g.write(compressed)
        print(f"Generated: {cached} ({len(compressed)} bytes)")
    bundle_entries.append((f"{name}.gz", ident, ctype, compressed))

bundle_dir = (
    scons_env.subst("$BUILD_DIR") if scons_env is not None else os.path.join(project_dir, ".pio", "web")
)
write_bundle(bundle_entries, bundle_dir)

if scons_env is not None:
    release_tag = get_release_tag(scons_env["PIOENV"], get_base_version(project_dir)[1])
    scons_env.Append(CPPDEFINES=[("WEB_BUNDLE_RELEASE_TAG", f'\\"{release_tag}\\"')])
    print(f"Web bundle release: {release_tag or '(none: not a published build)'}")
