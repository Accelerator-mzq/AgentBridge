"""对比 Spec 与当前关卡状态，生成执行计划。"""

from __future__ import annotations

from typing import Any


# L1 语义工具动作
ACTION_CREATE = "CREATE"
ACTION_UPDATE = "UPDATE"
ACTION_SKIP = "SKIP"

# L3 UI 工具动作
ACTION_UI_TOOL = "UI_TOOL"


def generate_plan(
    spec_actors: list[dict[str, Any]],
    existing_actors: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """
    对比 Spec Actor 列表与当前关卡 Actor 列表，生成执行计划。

    返回的每个计划条目格式：
    {
        "actor_spec": {...},
        "action": "CREATE" | "UPDATE" | "UI_TOOL" | "SKIP",
        "execution_method": "semantic" | "ui_tool",
        "existing_actor_path": None | "/Game/...",
        "reason": "人类可读的决策说明",
    }
    """
    existing_by_name = _index_existing_actors(existing_actors)
    plan: list[dict[str, Any]] = []

    for actor_spec in spec_actors:
        actor_id = str(actor_spec.get("id", ""))
        execution_method = actor_spec.get("execution_method", "semantic")

        # L3 UI 工具优先由 Spec 显式决定，不参与 CREATE/UPDATE 判定。
        if execution_method == "ui_tool":
            ui_action_type = actor_spec.get("ui_action", {}).get("type", "?")
            plan.append(
                {
                    "actor_spec": actor_spec,
                    "action": ACTION_UI_TOOL,
                    "execution_method": "ui_tool",
                    "existing_actor_path": None,
                    "reason": f"Actor '{actor_id}' uses L3 UI tool: {ui_action_type}",
                }
            )
            continue

        existing_actor = existing_by_name.get(actor_id)
        if existing_actor is not None:
            plan.append(
                {
                    "actor_spec": actor_spec,
                    "action": ACTION_UPDATE,
                    "execution_method": "semantic",
                    "existing_actor_path": existing_actor.get("actor_path"),
                    "reason": f"Actor '{actor_id}' already exists, will update transform",
                }
            )
        else:
            plan.append(
                {
                    "actor_spec": actor_spec,
                    "action": ACTION_CREATE,
                    "execution_method": "semantic",
                    "existing_actor_path": None,
                    "reason": f"Actor '{actor_id}' not found, will create",
                }
            )

    return plan


def _index_existing_actors(existing_actors: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """按 actor_name 建立现有关卡 Actor 索引。"""
    index: dict[str, dict[str, Any]] = {}

    for actor_info in existing_actors:
        actor_name = actor_info.get("actor_name")
        if isinstance(actor_name, str) and actor_name.strip():
            # 保留第一个命中的 Actor，避免重复名称时后值覆盖前值。
            index.setdefault(actor_name, actor_info)

    return index
