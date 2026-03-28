"""对写后读回的实际值进行结构化验证。"""

from __future__ import annotations

from copy import deepcopy
from typing import Any


DEFAULT_TOLERANCES = {
    "location": 0.01,
    "rotation": 0.01,
    "relative_scale3d": 0.001,
    "world_bounds_extent": 1.0,
}

L3_TOLERANCES = {
    "location": 100.0,
}

_VECTOR_COMPONENTS = {
    "location": ("X", "Y", "Z"),
    "rotation": ("Pitch", "Yaw", "Roll"),
    "relative_scale3d": ("X", "Y", "Z"),
    "collision_box_extent": ("X", "Y", "Z"),
}

_COLLISION_SCALAR_FIELDS = (
    "collision_profile_name",
    "collision_enabled",
    "can_affect_navigation",
    "generate_overlap_events",
)


def verify_transform(
    expected: dict[str, Any],
    actual: dict[str, Any],
    tolerances: dict[str, float] | None = None,
) -> dict[str, Any]:
    """逐字段比对 transform，输出结构化检查结果。"""
    merged_tolerances = _merge_tolerances(tolerances)
    checks: list[dict[str, Any]] = []
    mismatches: list[str] = []

    for field_name in ("location", "rotation", "relative_scale3d"):
        expected_vector = _coerce_vector3(expected.get(field_name))
        actual_vector = _coerce_vector3(actual.get(field_name))
        tolerance = float(merged_tolerances.get(field_name, 0.0))
        component_names = _VECTOR_COMPONENTS[field_name]

        for component_index, component_name in enumerate(component_names):
            expected_value = expected_vector[component_index]
            actual_value = actual_vector[component_index]
            delta = abs(expected_value - actual_value)
            passed = delta <= tolerance
            field_path = f"{field_name}.{component_name}"

            checks.append(
                {
                    "field": field_path,
                    "expected": expected_value,
                    "actual": actual_value,
                    "delta": delta,
                    "tolerance": tolerance,
                    "pass": passed,
                }
            )

            if not passed:
                mismatches.append(
                    f"{field_path}: expected={_format_number(expected_value)}, "
                    f"actual={_format_number(actual_value)}, "
                    f"delta={_format_number(delta)} > tolerance={_format_number(tolerance)}"
                )

    return {
        "status": "success" if not mismatches else "mismatch",
        "checks": checks,
        "mismatches": mismatches,
    }


def verify_actor_state(
    expected_spec: dict[str, Any],
    actual_response: dict[str, Any],
    execution_method: str = "semantic",
) -> dict[str, Any]:
    """完整校验 Actor 状态，自动选择 semantic / ui_tool 容差。"""
    actor_id = str(expected_spec.get("id", ""))
    transform_tolerances = (
        _merge_tolerances(L3_TOLERANCES)
        if execution_method == "ui_tool"
        else deepcopy(DEFAULT_TOLERANCES)
    )

    expected_transform = expected_spec.get("transform")
    actual_transform = actual_response.get("transform", {})
    if isinstance(expected_transform, dict):
        transform_check = verify_transform(
            expected=expected_transform,
            actual=actual_transform,
            tolerances=transform_tolerances,
        )
    else:
        transform_check = {
            "status": "success",
            "checks": [],
            "mismatches": [],
        }

    expected_class = expected_spec.get("class")
    actual_class = actual_response.get("class")
    class_check = _build_class_check(expected_class, actual_class)

    collision_checks, collision_mismatches = _verify_collision(
        expected_spec.get("collision"),
        actual_response.get("collision"),
    )

    mismatches = []
    mismatches.extend(transform_check["mismatches"])
    if not class_check["pass"]:
        mismatches.append(
            f"class: expected={expected_class}, actual={actual_class}"
        )
    mismatches.extend(collision_mismatches)

    return {
        "actor_id": actor_id,
        "status": "success" if not mismatches else "mismatch",
        "execution_method": execution_method,
        "transform_check": transform_check,
        "class_check": class_check,
        "collision_checks": collision_checks,
        "mismatches": mismatches,
    }


def _verify_collision(
    expected_collision: Any,
    actual_collision: Any,
) -> tuple[list[dict[str, Any]], list[str]]:
    """校验 collision 配置。"""
    if not isinstance(expected_collision, dict):
        return [], []

    checks: list[dict[str, Any]] = []
    mismatches: list[str] = []

    if not isinstance(actual_collision, dict):
        return checks, ["collision: actual collision data missing"]

    for field_name in _COLLISION_SCALAR_FIELDS:
        if field_name not in expected_collision:
            continue

        expected_value = expected_collision.get(field_name)
        actual_value = actual_collision.get(field_name)
        passed = expected_value == actual_value
        checks.append(
            {
                "field": f"collision.{field_name}",
                "expected": expected_value,
                "actual": actual_value,
                "pass": passed,
            }
        )
        if not passed:
            mismatches.append(
                f"collision.{field_name}: expected={expected_value}, actual={actual_value}"
            )

    if "collision_box_extent" in expected_collision:
        expected_extent = _coerce_vector3(expected_collision.get("collision_box_extent"))
        actual_extent = _coerce_vector3(actual_collision.get("collision_box_extent"))
        tolerance = float(DEFAULT_TOLERANCES["world_bounds_extent"])

        for index, component_name in enumerate(_VECTOR_COMPONENTS["collision_box_extent"]):
            expected_value = expected_extent[index]
            actual_value = actual_extent[index]
            delta = abs(expected_value - actual_value)
            passed = delta <= tolerance
            field_path = f"collision.collision_box_extent.{component_name}"

            checks.append(
                {
                    "field": field_path,
                    "expected": expected_value,
                    "actual": actual_value,
                    "delta": delta,
                    "tolerance": tolerance,
                    "pass": passed,
                }
            )

            if not passed:
                mismatches.append(
                    f"{field_path}: expected={_format_number(expected_value)}, "
                    f"actual={_format_number(actual_value)}, "
                    f"delta={_format_number(delta)} > tolerance={_format_number(tolerance)}"
                )

    return checks, mismatches


def _build_class_check(expected_class: Any, actual_class: Any) -> dict[str, Any]:
    """构造 class 精确匹配结果。"""
    if expected_class in (None, ""):
        return {
            "expected": expected_class,
            "actual": actual_class,
            "pass": True,
            "skipped": True,
        }

    return {
        "expected": expected_class,
        "actual": actual_class,
        "pass": expected_class == actual_class,
        "skipped": False,
    }


def _merge_tolerances(overrides: dict[str, float] | None) -> dict[str, float]:
    """合并默认容差与覆盖容差。"""
    merged = deepcopy(DEFAULT_TOLERANCES)
    if overrides:
        merged.update(overrides)
    return merged


def _coerce_vector3(value: Any) -> list[float]:
    """将输入规整为长度 3 的浮点列表。"""
    if isinstance(value, list) and len(value) == 3:
        return [float(component) for component in value]
    return [0.0, 0.0, 0.0]


def _format_number(value: float) -> str:
    """输出与验收文案一致的数字格式。"""
    return str(float(value))
