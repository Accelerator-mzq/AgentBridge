"""读取并校验结构化 YAML Spec。"""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError as exc:  # pragma: no cover - 仅在缺少依赖时触发
    raise ImportError("spec_reader.py 依赖 PyYAML，请先执行 `pip install pyyaml`。") from exc


TOP_LEVEL_KEYS = (
    "spec_version",
    "scene",
    "defaults",
    "layout",
    "anchors",
    "actors",
    "validation",
)

SUPPORTED_EXECUTION_METHODS = {"semantic", "ui_tool"}

UI_ACTION_REQUIRED_FIELDS = {
    "drag_asset_to_viewport": ("asset_path", "drop_location"),
    "click_detail_panel_button": ("actor_path", "button_label"),
    "type_in_detail_panel_field": ("actor_path", "property_path", "value"),
}

VECTOR_FIELDS = (
    "location",
    "rotation",
    "relative_scale3d",
)


def read_spec(spec_path: str) -> dict[str, Any]:
    """读取 YAML Spec，补齐默认字段，并返回标准化后的 dict。"""
    path = Path(spec_path)
    if not path.is_file():
        raise FileNotFoundError(f"Spec 文件不存在: {path}")

    with path.open("r", encoding="utf-8") as handle:
        loaded = yaml.safe_load(handle) or {}

    if not isinstance(loaded, dict):
        raise ValueError("Spec 顶层必须是 mapping/dict。")

    if "scene" not in loaded:
        raise ValueError("Spec 缺少必填顶层字段: scene")
    if "actors" not in loaded:
        raise ValueError("Spec 缺少必填顶层字段: actors")

    spec: dict[str, Any] = {
        "spec_version": loaded.get("spec_version", ""),
        "scene": deepcopy(loaded.get("scene") or {}),
        "defaults": deepcopy(loaded.get("defaults") or {}),
        "layout": deepcopy(loaded.get("layout") or {}),
        "anchors": deepcopy(loaded.get("anchors") or {}),
        "actors": _normalize_actors(loaded.get("actors")),
        "validation": deepcopy(loaded.get("validation") or {}),
    }

    if not isinstance(spec["scene"], dict):
        raise ValueError("Spec 的 scene 字段必须是 dict。")
    if not isinstance(spec["actors"], list):
        raise ValueError("Spec 的 actors 字段必须是 list。")
    if not isinstance(spec["validation"], dict):
        raise ValueError("Spec 的 validation 字段必须是 dict。")

    spec["validation"].setdefault("rules", [])
    if spec["validation"]["rules"] is None:
        spec["validation"]["rules"] = []

    return spec


def validate_spec(spec: dict[str, Any]) -> tuple[bool, list[str]]:
    """深度校验 Spec，返回 (是否合法, 错误列表)。"""
    errors: list[str] = []

    if not isinstance(spec, dict):
        return False, ["Spec 顶层必须是 dict。"]

    scene = spec.get("scene")
    if not isinstance(scene, dict):
        errors.append("scene 必须是 dict。")
        scene = {}

    scene_id = scene.get("scene_id")
    if not _is_non_empty_string(scene_id):
        errors.append("scene.scene_id 必须是非空字符串。")

    target_level = scene.get("target_level")
    if not _is_non_empty_string(target_level):
        errors.append("scene.target_level 必须是非空字符串。")
    elif not str(target_level).startswith("/Game/"):
        errors.append("scene.target_level 必须以 /Game/ 开头。")

    actors = spec.get("actors")
    if not isinstance(actors, list):
        return False, errors + ["actors 必须是 list。"]

    actor_ids: set[str] = set()
    for index, actor in enumerate(actors):
        actor_label = f"actors[{index}]"
        if not isinstance(actor, dict):
            errors.append(f"{actor_label} 必须是 dict。")
            continue

        actor_id = actor.get("id")
        if not _is_non_empty_string(actor_id):
            errors.append(f"{actor_label}.id 必须是非空字符串。")
            actor_name_for_msg = "?"
        else:
            actor_name_for_msg = str(actor_id)
            if actor_name_for_msg in actor_ids:
                errors.append(f"Actor id 重复: {actor_name_for_msg}")
            else:
                actor_ids.add(actor_name_for_msg)

        execution_method = actor.get("execution_method", "semantic")
        if not _is_non_empty_string(execution_method):
            errors.append(f"{actor_label}({actor_name_for_msg}).execution_method 必须是字符串。")
            execution_method = "semantic"
        elif execution_method not in SUPPORTED_EXECUTION_METHODS:
            errors.append(
                f"{actor_label}({actor_name_for_msg}).execution_method 必须是 semantic 或 ui_tool。"
            )

        if execution_method == "semantic":
            if not _is_non_empty_string(actor.get("class")):
                errors.append(f"{actor_label}({actor_name_for_msg}).class 必须是非空字符串。")
            errors.extend(
                _validate_transform(
                    actor.get("transform"),
                    owner=f"{actor_label}({actor_name_for_msg}).transform",
                )
            )
        elif execution_method == "ui_tool":
            errors.extend(
                _validate_ui_action(
                    actor.get("ui_action"),
                    owner=f"{actor_label}({actor_name_for_msg}).ui_action",
                )
            )
            # ui_tool 条目允许没有 transform；若显式提供，则仍按标准三元组校验。
            if "transform" in actor and actor.get("transform") is not None:
                errors.extend(
                    _validate_transform(
                        actor.get("transform"),
                        owner=f"{actor_label}({actor_name_for_msg}).transform",
                    )
                )

    validation = spec.get("validation", {})
    if not isinstance(validation, dict):
        errors.append("validation 必须是 dict。")
        validation = {}

    rules = validation.get("rules", [])
    if rules is None:
        rules = []
    if not isinstance(rules, list):
        errors.append("validation.rules 必须是 list。")
        rules = []

    for index, rule in enumerate(rules):
        if not isinstance(rule, dict):
            errors.append(f"validation.rules[{index}] 必须是 dict。")
            continue

        actor_id = rule.get("actor_id")
        if not _is_non_empty_string(actor_id):
            errors.append(f"validation.rules[{index}].actor_id 必须是非空字符串。")
            continue

        if actor_id not in actor_ids:
            errors.append(
                f"validation.rules[{index}].actor_id 引用了不存在的 actor: {actor_id}"
            )

    return len(errors) == 0, errors


def get_actors_by_execution_method(spec: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    """按 execution_method 分组返回 Actor 列表。"""
    groups: dict[str, list[dict[str, Any]]] = {
        "semantic": [],
        "ui_tool": [],
    }

    actors = spec.get("actors", [])
    if not isinstance(actors, list):
        return groups

    for actor in actors:
        if not isinstance(actor, dict):
            continue

        execution_method = actor.get("execution_method", "semantic")
        if execution_method not in groups:
            groups[execution_method] = []
        groups[execution_method].append(actor)

    return groups


def _normalize_actors(actors: Any) -> list[dict[str, Any]]:
    """为 Actor 列表补 execution_method 默认值，并保持输入数据不被原地修改。"""
    if actors is None:
        return []
    if not isinstance(actors, list):
        raise ValueError("Spec 的 actors 字段必须是 list。")

    normalized: list[dict[str, Any]] = []
    for index, actor in enumerate(actors):
        if not isinstance(actor, dict):
            raise ValueError(f"actors[{index}] 必须是 dict。")
        actor_copy = deepcopy(actor)
        actor_copy.setdefault("execution_method", "semantic")
        normalized.append(actor_copy)

    return normalized


def _validate_transform(transform: Any, owner: str) -> list[str]:
    """校验 transform 三元组字段。"""
    errors: list[str] = []

    if not isinstance(transform, dict):
        return [f"{owner} 必须是 dict。"]

    for field_name in VECTOR_FIELDS:
        vector = transform.get(field_name)
        errors.extend(_validate_vector3(vector, f"{owner}.{field_name}"))

    scale = transform.get("relative_scale3d")
    if isinstance(scale, list) and len(scale) == 3:
        if any(value == 0 for value in scale):
            errors.append(f"{owner}.relative_scale3d 的每个分量都必须非零。")

    return errors


def _validate_ui_action(ui_action: Any, owner: str) -> list[str]:
    """校验 ui_tool 所需的 ui_action 配置。"""
    errors: list[str] = []

    if not isinstance(ui_action, dict):
        return [f"{owner} 必须是 dict。"]

    action_type = ui_action.get("type")
    if not _is_non_empty_string(action_type):
        return [f"{owner}.type 必须是非空字符串。"]

    required_fields = UI_ACTION_REQUIRED_FIELDS.get(str(action_type), ())
    for field_name in required_fields:
        value = ui_action.get(field_name)
        if field_name == "drop_location":
            errors.extend(_validate_vector3(value, f"{owner}.{field_name}"))
        elif not _is_non_empty_string(value):
            errors.append(f"{owner}.{field_name} 必须是非空字符串。")

    return errors


def _validate_vector3(value: Any, owner: str) -> list[str]:
    """校验长度为 3 的数值列表。"""
    if not isinstance(value, list):
        return [f"{owner} 必须是 3 元素列表。"]
    if len(value) != 3:
        return [f"{owner} 必须是 3 元素列表。"]
    if not all(isinstance(item, (int, float)) for item in value):
        return [f"{owner} 的 3 个元素都必须是数值。"]
    return []


def _is_non_empty_string(value: Any) -> bool:
    """检查是否为非空字符串。"""
    return isinstance(value, str) and value.strip() != ""
