"""
validate_examples.py
====================
AGENT + UE5 可操作层 — Schema/Example 校验脚本。

职责：
  - 读取 agent_ue5/schemas/examples/ 下的 example JSON 文件
  - 用对应的 Schema 文件校验 example 是否合法
  - 输出结构化校验结果

使用方式：
  python validate_examples.py              # 校验全部 example
  python validate_examples.py --strict     # 严格模式（未映射/缺失 schema 计为失败）
  python validate_examples.py --example get_actor_state.example.json  # 校验单个
  python validate_examples.py --list       # 列出所有映射关系

前提：
  - 脚本位于 PythonPackage/agent_ue5/validators/
  - Schema 文件位于 PythonPackage/agent_ue5/schemas/
  - 所有 Schema 文件不使用 $id（避免远程解析）
  - 依赖：pip install jsonschema
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from jsonschema import Draft202012Validator, RefResolver
from jsonschema.exceptions import ValidationError, SchemaError


# =========================
# Configuration
# =========================

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
SCHEMAS_DIR = PACKAGE_ROOT / "schemas"
EXAMPLES_DIR = SCHEMAS_DIR / "examples"

# 显式映射：example 文件名 -> schema 文件相对路径（相对于 SCHEMAS_DIR）
# 覆盖 MVP 全部 9 个反馈接口 + 1 个写后反馈 = 10 个 example
EXAMPLE_TO_SCHEMA: Dict[str, str] = {
    # === 反馈接口 example ===
    "get_current_project_state.example.json":
        "feedback/project/get_current_project_state.response.schema.json",
    "list_level_actors.example.json":
        "feedback/level/list_level_actors.response.schema.json",
    "get_actor_state.example.json":
        "feedback/actor/get_actor_state.response.schema.json",
    "get_actor_bounds.example.json":
        "feedback/actor/get_actor_bounds.response.schema.json",
    "get_component_state.example.json":
        "feedback/actor/get_component_state.response.schema.json",
    "get_material_assignment.example.json":
        "feedback/actor/get_material_assignment.response.schema.json",
    "get_asset_metadata.example.json":
        "feedback/asset/get_asset_metadata.response.schema.json",
    "get_dirty_assets.example.json":
        "feedback/asset/get_dirty_assets.response.schema.json",
    "run_map_check.example.json":
        "feedback/validation/run_map_check.response.schema.json",

    # === 写后反馈 example ===
    "write_operation_feedback.example.json":
        "write_feedback/write_operation_feedback.response.schema.json",
}

# 这些 example 默认跳过自动 schema 校验，仅作为参考样例。
# common schema 的 root 是 $defs 集合，不是直接可校验的完整对象。
REFERENCE_ONLY_EXAMPLES = {
    "primitives.example.json",
    "transform.example.json",
    "bounds.example.json",
    "error.example.json",
}


# =========================
# Data classes
# =========================

@dataclass
class ValidationResult:
    example_path: Path
    schema_path: Optional[Path]
    ok: bool
    message: str


# =========================
# Helpers
# =========================

def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def format_validation_error(err: ValidationError) -> str:
    path_str = ".".join(str(p) for p in err.absolute_path)
    schema_path_str = " -> ".join(str(p) for p in err.absolute_schema_path)
    parts = [
        f"message: {err.message}",
        f"instance_path: {path_str or '<root>'}",
        f"schema_path: {schema_path_str or '<root>'}",
    ]
    return " | ".join(parts)


def check_no_dollar_id(schema_obj: dict, schema_path: Path) -> Optional[str]:
    """
    检查 Schema 是否包含 $id。
    首批 Schema 不应使用 $id，避免 $ref 被解析到远程 URL。
    """
    if "$id" in schema_obj:
        return (
            f"Schema 包含 $id: {schema_obj['$id']}。"
            f"首批 Schema 不应使用 $id，请删除以避免远程解析。"
        )
    return None


def make_resolver(schema_path: Path, schema_obj: dict) -> RefResolver:
    """
    为相对 $ref 提供本地文件解析能力。
    base_uri 基于 schema 文件的文件系统路径。
    前提：schema 不包含 $id（否则 $id 会覆盖 base_uri）。
    """
    base_uri = schema_path.resolve().as_uri()
    return RefResolver(base_uri=base_uri, referrer=schema_obj)


def validate_example_against_schema(
    example_path: Path,
    schema_path: Path,
) -> ValidationResult:
    # 读取 schema
    try:
        schema_obj = load_json(schema_path)
    except Exception as e:
        return ValidationResult(
            example_path=example_path,
            schema_path=schema_path,
            ok=False,
            message=f"无法读取 schema: {e}",
        )

    # 检查 $id
    id_warning = check_no_dollar_id(schema_obj, schema_path)
    if id_warning:
        return ValidationResult(
            example_path=example_path,
            schema_path=schema_path,
            ok=False,
            message=id_warning,
        )

    # 读取 example
    try:
        example_obj = load_json(example_path)
    except Exception as e:
        return ValidationResult(
            example_path=example_path,
            schema_path=schema_path,
            ok=False,
            message=f"无法读取 example JSON: {e}",
        )

    # 校验 schema 自身合法性
    try:
        Draft202012Validator.check_schema(schema_obj)
    except SchemaError as e:
        return ValidationResult(
            example_path=example_path,
            schema_path=schema_path,
            ok=False,
            message=f"schema 非法: {e.message}",
        )

    # 校验 example 是否符合 schema
    try:
        resolver = make_resolver(schema_path, schema_obj)
        validator = Draft202012Validator(schema_obj, resolver=resolver)
        errors = sorted(
            validator.iter_errors(example_obj),
            key=lambda x: list(x.absolute_path),
        )
        if errors:
            first = errors[0]
            return ValidationResult(
                example_path=example_path,
                schema_path=schema_path,
                ok=False,
                message=f"[{len(errors)} error(s)] {format_validation_error(first)}",
            )
    except Exception as e:
        return ValidationResult(
            example_path=example_path,
            schema_path=schema_path,
            ok=False,
            message=f"校验执行异常: {e}",
        )

    return ValidationResult(
        example_path=example_path,
        schema_path=schema_path,
        ok=True,
        message="校验通过",
    )


def discover_examples(example_name: Optional[str] = None) -> List[Path]:
    if not EXAMPLES_DIR.exists():
        return []
    if example_name:
        target = EXAMPLES_DIR / example_name
        return [target] if target.exists() else []
    return sorted(EXAMPLES_DIR.glob("*.json"))


def resolve_schema_for_example(
    example_path: Path,
) -> Tuple[Optional[Path], Optional[str]]:
    name = example_path.name
    if name in REFERENCE_ONLY_EXAMPLES:
        return None, "reference_only"
    rel = EXAMPLE_TO_SCHEMA.get(name)
    if rel is None:
        return None, "unmapped"
    schema_path = SCHEMAS_DIR / rel
    if not schema_path.exists():
        return None, "schema_missing"
    return schema_path, None


def rel_path(path: Path) -> str:
    try:
        return str(path.relative_to(PACKAGE_ROOT))
    except ValueError:
        return str(path)


def print_result(result: ValidationResult) -> None:
    icon = "[OK]" if result.ok else "[ERROR]"
    schema_str = rel_path(result.schema_path) if result.schema_path else "<none>"
    example_str = rel_path(result.example_path)
    print(f"{icon} {example_str}")
    print(f"      schema: {schema_str}")
    print(f"      detail: {result.message}")


def print_mapping_list() -> None:
    print("========== Example -> Schema Mapping ==========")
    for example_name, schema_rel in sorted(EXAMPLE_TO_SCHEMA.items()):
        schema_path = SCHEMAS_DIR / schema_rel
        exists = "OK" if schema_path.exists() else "MISSING"
        print(f"  {example_name}")
        print(f"    -> {schema_rel} [{exists}]")
    print(f"\nReference-only (skipped): {sorted(REFERENCE_ONLY_EXAMPLES)}")


# =========================
# Main
# =========================

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate example JSON files against schema files. "
        "AGENT + UE5 可操作层 Schema 校验工具。",
    )
    parser.add_argument(
        "--example",
        help="只校验某个 example 文件名，例如 get_actor_state.example.json",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="严格模式：未映射的 example 或缺失 schema 直接计为失败",
    )
    parser.add_argument(
        "--include-reference-only",
        action="store_true",
        help="显示 reference-only examples（默认只提示跳过）",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="列出所有 example -> schema 映射关系，不执行校验",
    )
    args = parser.parse_args()

    if args.list:
        print_mapping_list()
        return 0

    examples = discover_examples(args.example)
    if not examples:
        print("未找到 example 文件。")
        print(f"  查找目录: {EXAMPLES_DIR}")
        return 1

    results: List[ValidationResult] = []
    skipped_reference_only: List[Path] = []
    unmapped_examples: List[Path] = []
    missing_schema_examples: List[Path] = []

    for example_path in examples:
        schema_path, reason = resolve_schema_for_example(example_path)

        if reason == "reference_only":
            skipped_reference_only.append(example_path)
            continue

        if reason == "unmapped":
            unmapped_examples.append(example_path)
            continue

        if reason == "schema_missing":
            missing_schema_examples.append(example_path)
            continue

        assert schema_path is not None
        result = validate_example_against_schema(example_path, schema_path)
        results.append(result)
        print_result(result)

    # Reference-only
    if skipped_reference_only and args.include_reference_only:
        print("\n[INFO] 以下 example 仅作为参考样例，默认未纳入自动 schema 校验：")
        for p in skipped_reference_only:
            print(f"  - {rel_path(p)}")

    # Unmapped
    if unmapped_examples:
        print("\n[WARN] 以下 example 未配置 schema 映射：")
        for p in unmapped_examples:
            print(f"  - {rel_path(p)}")

    # Missing schema
    if missing_schema_examples:
        print("\n[WARN] 以下 example 的 schema 文件不存在：")
        for p in missing_schema_examples:
            name = p.name
            expected = EXAMPLE_TO_SCHEMA.get(name, "?")
            print(f"  - {rel_path(p)} -> {expected}")

    # Summary
    total_checked = len(results)
    total_passed = sum(1 for r in results if r.ok)
    total_failed = sum(1 for r in results if not r.ok)

    print("\n========== Summary ==========")
    print(f"Package root           : {PACKAGE_ROOT}")
    print(f"Schemas dir            : {SCHEMAS_DIR}")
    print(f"Examples dir           : {EXAMPLES_DIR}")
    print(f"Checked examples       : {total_checked}")
    print(f"Passed                 : {total_passed}")
    print(f"Failed                 : {total_failed}")
    print(f"Reference-only skipped : {len(skipped_reference_only)}")
    print(f"Unmapped examples      : {len(unmapped_examples)}")
    print(f"Missing schema targets : {len(missing_schema_examples)}")

    exit_code = 0

    if total_failed > 0:
        exit_code = 1

    if args.strict and (unmapped_examples or missing_schema_examples):
        exit_code = 1

    if total_checked > 0 and total_failed == 0 and not unmapped_examples and not missing_schema_examples:
        print("\n[SUCCESS] 全部 example 校验通过，本地校验链正常。")

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
