#!/usr/bin/env python3
"""
Validate that feature metadata stays in sync across build tooling.
"""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def find_assigned_value(tree: ast.AST, variable_name: str, path: Path) -> ast.AST:
    for node in tree.body:  # type: ignore[attr-defined]
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == variable_name:
                    return node.value
    raise ValueError(f"{path}: {variable_name} assignment not found")


def parse_const_str(node: ast.AST, context: str) -> str:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    raise ValueError(f"{context}: expected string literal")


def parse_const_bool(node: ast.AST, context: str) -> bool:
    if isinstance(node, ast.Constant) and isinstance(node.value, bool):
        return node.value
    raise ValueError(f"{context}: expected boolean literal")


def parse_const_int(node: ast.AST, context: str) -> int:
    if isinstance(node, ast.Constant) and isinstance(node.value, int):
        return node.value
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        inner = parse_const_int(node.operand, context)
        return -inner
    raise ValueError(f"{context}: expected integer literal")


def parse_python_dict_keys(path: Path, variable_name: str) -> list[str]:
    tree = ast.parse(path.read_text(), filename=str(path))
    value = find_assigned_value(tree, variable_name, path)
    if not isinstance(value, ast.Dict):
        raise ValueError(f"{path}: {variable_name} is not a dict literal")

    keys: list[str] = []
    for key in value.keys:
        keys.append(parse_const_str(key, f"{path}: {variable_name} key"))
    return keys


def parse_python_list(path: Path, variable_name: str) -> list[str]:
    tree = ast.parse(path.read_text(), filename=str(path))
    value = find_assigned_value(tree, variable_name, path)
    if not isinstance(value, (ast.List, ast.Tuple)):
        raise ValueError(f"{path}: {variable_name} is not a list/tuple literal")

    values: list[str] = []
    for element in value.elts:
        values.append(parse_const_str(element, f"{path}: {variable_name} element"))
    return values


def parse_python_feature_specs(path: Path) -> dict[str, dict[str, int | str]]:
    tree = ast.parse(path.read_text(), filename=str(path))
    value = find_assigned_value(tree, "FEATURES", path)
    if not isinstance(value, ast.Dict):
        raise ValueError(f"{path}: FEATURES is not a dict literal")

    specs: dict[str, dict[str, int | str]] = {}
    for key_node, feature_node in zip(value.keys, value.values):
        key = parse_const_str(key_node, f"{path}: FEATURES key")
        if not isinstance(feature_node, ast.Call):
            raise ValueError(f"{path}: FEATURES[{key}] is not a Feature(...) call")

        kwargs = {kw.arg: kw.value for kw in feature_node.keywords if kw.arg is not None}
        flag_node = kwargs.get("flag")
        size_node = kwargs.get("size_kb")
        if flag_node is None or size_node is None:
            raise ValueError(f"{path}: FEATURES[{key}] is missing flag or size_kb")

        specs[key] = {
            "flag": parse_const_str(flag_node, f"{path}: FEATURES[{key}].flag"),
            "size_kb": parse_const_int(size_node, f"{path}: FEATURES[{key}].size_kb"),
        }

    return specs


def parse_python_profiles(path: Path) -> dict[str, dict[str, bool]]:
    tree = ast.parse(path.read_text(), filename=str(path))
    value = find_assigned_value(tree, "PROFILES", path)
    if not isinstance(value, ast.Dict):
        raise ValueError(f"{path}: PROFILES is not a dict literal")

    profiles: dict[str, dict[str, bool]] = {}
    for profile_key_node, profile_value_node in zip(value.keys, value.values):
        profile_key = parse_const_str(profile_key_node, f"{path}: PROFILES key")
        if not isinstance(profile_value_node, ast.Dict):
            raise ValueError(f"{path}: PROFILES[{profile_key}] is not a dict literal")

        fields: dict[str, ast.AST] = {}
        for field_key_node, field_value_node in zip(profile_value_node.keys, profile_value_node.values):
            field_key = parse_const_str(field_key_node, f"{path}: PROFILES[{profile_key}] field key")
            fields[field_key] = field_value_node

        feature_values: dict[str, bool] = {}
        features_node = fields.get("features")
        if features_node is None:
            raise ValueError(f"{path}: PROFILES[{profile_key}] missing features")
        if not isinstance(features_node, ast.Dict):
            raise ValueError(f"{path}: PROFILES[{profile_key}].features is not a dict")

        for feature_key_node, feature_value_node in zip(features_node.keys, features_node.values):
            feature_key = parse_const_str(
                feature_key_node, f"{path}: PROFILES[{profile_key}].features key"
            )
            feature_values[feature_key] = parse_const_bool(
                feature_value_node, f"{path}: PROFILES[{profile_key}].features[{feature_key}]"
            )

        profiles[profile_key] = feature_values

    return profiles


def parse_python_feature_dependencies(path: Path) -> dict[str, list[str]]:
    tree = ast.parse(path.read_text(), filename=str(path))
    value = find_assigned_value(tree, "FEATURE_METADATA", path)
    if not isinstance(value, ast.Dict):
        raise ValueError(f"{path}: FEATURE_METADATA is not a dict literal")

    dependencies: dict[str, list[str]] = {}
    for key_node, metadata_node in zip(value.keys, value.values):
        feature_key = parse_const_str(key_node, f"{path}: FEATURE_METADATA key")
        if not isinstance(metadata_node, ast.Call):
            raise ValueError(f"{path}: FEATURE_METADATA[{feature_key}] is not a FeatureMetadata(...) call")

        kwargs = {kw.arg: kw.value for kw in metadata_node.keywords if kw.arg is not None}
        requires_node = kwargs.get("requires")
        if requires_node is None:
            dependencies[feature_key] = []
            continue
        if not isinstance(requires_node, (ast.List, ast.Tuple)):
            raise ValueError(f"{path}: FEATURE_METADATA[{feature_key}].requires is not a list")

        requires: list[str] = []
        for req_node in requires_node.elts:
            requires.append(parse_const_str(req_node, f"{path}: FEATURE_METADATA[{feature_key}].requires"))
        dependencies[feature_key] = sorted(requires)

    return dependencies


def parse_shell_features(path: Path) -> list[str]:
    text = path.read_text()
    match = re.search(r"^\s*FEATURES=\(([^)]*)\)", text, re.M)
    if not match:
        raise ValueError(f"{path}: FEATURES=(...) not found")

    values: list[str] = []
    for dq, sq in re.findall(r'"([^"]+)"|\'([^\']+)\'', match.group(1)):
        values.append(dq or sq)

    if not values:
        raise ValueError(f"{path}: FEATURES list is empty")
    return values


def parse_workflow_inputs(path: Path) -> list[str]:
    text = path.read_text()
    # Find inputs under workflow_dispatch
    match = re.search(r"inputs:\s*\n((?:\s{6}\w+:\s*\n(?:\s{8}.*\n?)*)+)", text)
    if not match:
        raise ValueError(f"{path}: workflow inputs not found")

    inputs = re.findall(r"^\s{6}(\w+):", match.group(1), re.M)
    if "profile" in inputs:
        inputs.remove("profile")
    return sorted(inputs)


def parse_workflow_cmd_enables(path: Path) -> list[str]:
    text = path.read_text()
    enables = re.findall(r"CMD\+=\(--enable\s+(\w+)\)", text)
    return sorted(enables)


def extract_js_object_block(text: str, marker: str, path: Path) -> str:
    start = text.find(marker)
    if start == -1:
        raise ValueError(f"{path}: '{marker}' block not found")

    index = start + len(marker)
    depth = 1
    while index < len(text) and depth > 0:
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
        index += 1

    if depth != 0:
        raise ValueError(f"{path}: unbalanced braces while parsing marker {marker!r}")

    return text[start + len(marker) : index - 1]


def parse_js_top_level_entries(block: str) -> dict[str, str]:
    entries: dict[str, str] = {}
    index = 0
    key_pattern = re.compile(r"\s*([a-z_][a-z0-9_]*)\s*:\s*", re.A)

    while index < len(block):
        while index < len(block) and block[index] in " \t\r\n,":
            index += 1
        if index >= len(block):
            break

        match = key_pattern.match(block, index)
        if not match:
            raise ValueError(f"Unexpected token in JS object near: {block[index:index + 40]!r}")

        key = match.group(1)
        index = match.end()

        while index < len(block) and block[index].isspace():
            index += 1
        if index >= len(block):
            raise ValueError(f"Unexpected end of JS object after key {key!r}")

        if block[index] == "{":
            value_start = index
            depth = 1
            index += 1
            while index < len(block) and depth > 0:
                char = block[index]
                if char == "{":
                    depth += 1
                elif char == "}":
                    depth -= 1
                index += 1
            if depth != 0:
                raise ValueError(f"Unbalanced braces while parsing key {key!r}")
            value = block[value_start:index]
        else:
            value_start = index
            while index < len(block) and block[index] not in ",\n":
                index += 1
            value = block[value_start:index].strip()

        entries[key] = value

    return entries


def parse_configurator_features(path: Path) -> dict[str, dict[str, int | str]]:
    text = path.read_text()
    block = extract_js_object_block(text, "const FEATURES = {", path)
    entries = parse_js_top_level_entries(block)

    specs: dict[str, dict[str, int | str]] = {}
    for key, value in entries.items():
        if not value.startswith("{"):
            raise ValueError(f"{path}: FEATURES[{key}] is not an object")

        flag_match = re.search(r'\bflag\s*:\s*"([^"]+)"', value)
        size_match = re.search(r"\bsizeKb\s*:\s*(-?\d+)", value)
        if not flag_match or not size_match:
            raise ValueError(f"{path}: FEATURES[{key}] missing flag or sizeKb")

        specs[key] = {
            "flag": flag_match.group(1),
            "size_kb": int(size_match.group(1)),
        }

    if not specs:
        raise ValueError(f"{path}: no features parsed")
    return specs


def parse_configurator_profiles(path: Path) -> dict[str, dict[str, bool]]:
    text = path.read_text()
    block = extract_js_object_block(text, "const PROFILES = {", path)
    entries = parse_js_top_level_entries(block)

    profiles: dict[str, dict[str, bool]] = {}
    for profile_name, value in entries.items():
        if not value.startswith("{"):
            raise ValueError(f"{path}: PROFILES[{profile_name}] is not an object")

        features: dict[str, bool] = {}
        for feature, enabled in re.findall(r"\b([a-z_][a-z0-9_]*)\s*:\s*(true|false)\b", value):
            features[feature] = enabled == "true"

        profiles[profile_name] = features

    return profiles


def parse_configurator_dependencies(path: Path) -> dict[str, list[str]]:
    text = path.read_text()
    block = extract_js_object_block(text, "const FEATURE_DEPENDENCIES = {", path)
    dependencies: dict[str, list[str]] = {}

    for feature, deps_raw in re.findall(r"([a-z_][a-z0-9_]*)\s*:\s*\[([^\]]*)\]", block):
        deps = [match for match in re.findall(r'"([^"]+)"', deps_raw)]
        dependencies[feature] = sorted(deps)

    return dependencies


def check_duplicates(keys: list[str]) -> list[str]:
    seen: set[str] = set()
    duplicates: list[str] = []
    for key in keys:
        if key in seen:
            duplicates.append(key)
        seen.add(key)
    return duplicates


def compare_feature_sets(reference: list[str], candidate: list[str], name: str) -> list[str]:
    errors: list[str] = []
    reference_set = set(reference)
    candidate_set = set(candidate)

    missing = sorted(reference_set - candidate_set)
    extra = sorted(candidate_set - reference_set)
    if missing:
        errors.append(f"{name}: missing keys: {', '.join(missing)}")
    if extra:
        errors.append(f"{name}: unexpected keys: {', '.join(extra)}")

    duplicates = check_duplicates(candidate)
    if duplicates:
        errors.append(f"{name}: duplicate keys: {', '.join(sorted(set(duplicates)))}")

    return errors


def compare_feature_specs(
    reference: dict[str, dict[str, int | str]],
    candidate: dict[str, dict[str, int | str]],
    name: str,
) -> list[str]:
    errors: list[str] = []
    for key in sorted(reference.keys()):
        if key not in candidate:
            continue
        for field in ("flag", "size_kb"):
            if reference[key][field] != candidate[key][field]:
                errors.append(
                    f"{name}: {key}.{field} mismatch: "
                    f"expected {reference[key][field]!r}, got {candidate[key][field]!r}"
                )
    return errors


def compare_profiles(
    reference: dict[str, dict[str, bool]],
    candidate: dict[str, dict[str, bool]],
    name: str,
) -> list[str]:
    errors: list[str] = []

    missing_profiles = sorted(set(reference) - set(candidate))
    extra_profiles = sorted(set(candidate) - set(reference))
    if missing_profiles:
        errors.append(f"{name}: missing profiles: {', '.join(missing_profiles)}")
    if extra_profiles:
        errors.append(f"{name}: unexpected profiles: {', '.join(extra_profiles)}")

    for profile in sorted(set(reference) & set(candidate)):
        expected_enabled = {k for k, v in reference[profile].items() if v}
        actual_enabled = {k for k, v in candidate[profile].items() if v}
        missing_features = sorted(expected_enabled - actual_enabled)
        extra_features = sorted(actual_enabled - expected_enabled)
        if missing_features or extra_features:
            details: list[str] = []
            if missing_features:
                details.append(f"missing {', '.join(missing_features)}")
            if extra_features:
                details.append(f"extra {', '.join(extra_features)}")
            errors.append(f"{name}: profile {profile} mismatch ({'; '.join(details)})")

    return errors


def compare_dependencies(
    reference: dict[str, list[str]],
    candidate: dict[str, list[str]],
    name: str,
) -> list[str]:
    errors: list[str] = []

    expected_non_empty = {feature: deps for feature, deps in reference.items() if deps}
    missing = sorted(set(expected_non_empty) - set(candidate))
    extra = sorted(set(candidate) - set(expected_non_empty))
    if missing:
        errors.append(f"{name}: missing dependency entries: {', '.join(missing)}")
    if extra:
        errors.append(f"{name}: unexpected dependency entries: {', '.join(extra)}")

    for feature in sorted(set(expected_non_empty) & set(candidate)):
        expected_deps = sorted(expected_non_empty[feature])
        actual_deps = sorted(candidate[feature])
        if expected_deps != actual_deps:
            errors.append(
                f"{name}: {feature} dependency mismatch: "
                f"expected {expected_deps}, got {actual_deps}"
            )

    return errors


def main() -> int:
    reference_file = ROOT / "scripts" / "generate_build_config.py"
    reference_features = parse_python_dict_keys(reference_file, "FEATURES")
    reference_specs = parse_python_feature_specs(reference_file)
    reference_profiles = parse_python_profiles(reference_file)
    reference_dependencies = parse_python_feature_dependencies(reference_file)

    configurator_file = ROOT / "docs" / "configurator" / "index.html"
    configurator_specs = parse_configurator_features(configurator_file)
    configurator_profiles = parse_configurator_profiles(configurator_file)
    configurator_dependencies = parse_configurator_dependencies(configurator_file)

    sources = {
        "measure_feature_sizes.py": parse_python_list(ROOT / "scripts" / "measure_feature_sizes.py", "FEATURES"),
        "test_all_combinations.sh": parse_shell_features(ROOT / "scripts" / "test_all_combinations.sh"),
        "docs/configurator/index.html": list(configurator_specs.keys()),
    }

    errors: list[str] = []
    reference_duplicates = check_duplicates(reference_features)
    if reference_duplicates:
        errors.append(
            "generate_build_config.py: duplicate keys: "
            + ", ".join(sorted(set(reference_duplicates)))
        )

    for name, keys in sources.items():
        errors.extend(compare_feature_sets(reference_features, keys, name))

    errors.extend(
        compare_feature_specs(reference_specs, configurator_specs, "docs/configurator/index.html FEATURES")
    )
    errors.extend(
        compare_profiles(reference_profiles, configurator_profiles, "docs/configurator/index.html PROFILES")
    )
    errors.extend(
        compare_dependencies(
            reference_dependencies,
            configurator_dependencies,
            "docs/configurator/index.html FEATURE_DEPENDENCIES",
        )
    )

    if errors:
        print("Feature synchronization check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(
        "Feature synchronization check passed: "
        f"{len(reference_features)} features aligned across tooling and configurator metadata."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
