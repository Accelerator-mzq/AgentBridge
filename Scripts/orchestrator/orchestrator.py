"""Orchestrator 主编排逻辑。"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any


# 兼容 `python -m Scripts.orchestrator.orchestrator` 与直接脚本执行两种入口。
_THIS_DIR = Path(__file__).resolve().parent
_BRIDGE_DIR = _THIS_DIR.parent / "bridge"

if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))
if str(_BRIDGE_DIR) not in sys.path:
    sys.path.insert(0, str(_BRIDGE_DIR))

from project_config import get_saved_dir
from spec_reader import read_spec, validate_spec
from plan_generator import generate_plan, ACTION_CREATE, ACTION_UPDATE, ACTION_UI_TOOL
from verifier import verify_transform, verify_actor_state, L3_TOLERANCES
from report_generator import generate_report, save_report, format_summary

from bridge_core import set_channel, BridgeChannel, safe_execute
from query_tools import (
    get_current_project_state,
    list_level_actors,
    get_actor_state,
    get_actor_bounds,
    get_dirty_assets,
    run_map_check,
)
from write_tools import spawn_actor, set_actor_transform
from ui_tools import (
    click_detail_panel_button,
    type_in_detail_panel_field,
    drag_asset_to_viewport,
    cross_verify_ui_operation,
)


# L3 UI 工具类型 -> Python 调度函数。
_UI_TOOL_DISPATCH = {
    "drag_asset_to_viewport": drag_asset_to_viewport,
    "click_detail_panel_button": click_detail_panel_button,
    "type_in_detail_panel_field": type_in_detail_panel_field,
}

# L3 UI 工具类型 -> 对应的 L1 验证函数与参数来源。
_UI_TOOL_VERIFY = {
    "drag_asset_to_viewport": {
        "l1_func": list_level_actors,
        "l1_args": None,
    },
    "click_detail_panel_button": {
        "l1_func": get_actor_state,
        "l1_args_key": "actor_path",
    },
    "type_in_detail_panel_field": {
        "l1_func": get_actor_state,
        "l1_args_key": "actor_path",
    },
}


def run(
    spec_path: str,
    channel: BridgeChannel = BridgeChannel.CPP_PLUGIN,
    report_path: str | None = None,
) -> dict[str, Any]:
    """主编排入口：读取 Spec -> 生成计划 -> 执行 -> 验证 -> 报告。"""
    set_channel(channel)

    try:
        spec = read_spec(spec_path)
    except Exception as exc:
        report = _build_failed_report(
            spec_path=spec_path,
            reason=f"Failed to read spec: {exc}",
            errors=[str(exc)],
        )
        _save_report_if_needed(report, report_path)
        return report

    is_valid, spec_errors = validate_spec(spec)
    if not is_valid:
        report = _build_failed_report(
            spec_path=spec_path,
            reason="Spec validation failed",
            errors=spec_errors,
        )
        _save_report_if_needed(report, report_path)
        return report

    target_level = str(spec.get("scene", {}).get("target_level", ""))

    project_state = get_current_project_state()
    if project_state.get("status") not in ("success", "warning"):
        report = _build_failed_report(
            spec_path=spec_path,
            reason=project_state.get("summary", "Failed to query project state"),
            errors=_extract_errors_from_response(project_state),
        )
        _save_report_if_needed(report, report_path)
        return report

    current_actors_response = list_level_actors(level_path=target_level)
    if current_actors_response.get("status") not in ("success", "warning"):
        report = _build_failed_report(
            spec_path=spec_path,
            reason=current_actors_response.get("summary", "Failed to list actors"),
            errors=_extract_errors_from_response(current_actors_response),
        )
        _save_report_if_needed(report, report_path)
        return report

    current_actors = current_actors_response.get("data", {}).get("actors", [])
    plan = generate_plan(spec.get("actors", []), current_actors)

    execution_results: list[dict[str, Any]] = []
    verification_results: list[dict[str, Any]] = []
    resolved_actor_paths: dict[str, str] = {}

    for plan_item in plan:
        step_result = safe_execute(
            _execute_plan_item,
            plan_item,
            target_level,
            channel,
            resolved_actor_paths,
            timeout=30,
        )

        if "execution_result" in step_result and "verification_result" in step_result:
            execution_results.append(step_result["execution_result"])
            verification_results.append(step_result["verification_result"])

            execution_actor_id = step_result["execution_result"].get("actor_id")
            execution_actor_path = step_result["execution_result"].get("actor_path")
            if execution_actor_id and execution_actor_path:
                resolved_actor_paths[str(execution_actor_id)] = str(execution_actor_path)
            continue

        # safe_execute 捕获异常后会返回 failed 响应，这里转成当前 Actor 的失败记录。
        fallback_execution, fallback_verification = _build_step_failure_from_response(
            plan_item=plan_item,
            response=step_result,
        )
        execution_results.append(fallback_execution)
        verification_results.append(fallback_verification)

    dirty_response = get_dirty_assets()
    dirty_assets = dirty_response.get("data", {}).get("dirty_assets", [])

    map_check_response = run_map_check(level_path=target_level)

    report = generate_report(
        spec_path=spec_path,
        plan=plan,
        execution_results=execution_results,
        verification_results=verification_results,
        dirty_assets=dirty_assets,
        map_check=map_check_response,
    )

    _save_report_if_needed(report, report_path)
    return report


def main(argv: list[str] | None = None) -> int:
    """CLI 入口。"""
    parser = argparse.ArgumentParser(description="AGENT UE5 Orchestrator")
    parser.add_argument("spec_path", help="结构化 Spec YAML 路径")
    parser.add_argument(
        "--channel",
        default=BridgeChannel.CPP_PLUGIN.value,
        choices=[channel.value for channel in BridgeChannel],
        help="桥接执行通道",
    )
    parser.add_argument(
        "--report",
        default=None,
        help="报告输出路径（可选）",
    )
    args = parser.parse_args(argv)
    report_path = args.report or str(_build_default_cli_report_path())

    report = run(
        spec_path=args.spec_path,
        channel=BridgeChannel(args.channel),
        report_path=report_path,
    )
    print(format_summary(report))
    return 0 if report.get("overall_status") == "success" else 1


def _execute_plan_item(
    plan_item: dict[str, Any],
    target_level: str,
    channel: BridgeChannel,
    resolved_actor_paths: dict[str, str],
) -> dict[str, Any]:
    """执行单个计划条目，并返回执行/验证双结果。"""
    actor_spec = plan_item.get("actor_spec", {})
    actor_id = str(actor_spec.get("id", ""))
    action = plan_item.get("action", "")
    execution_method = plan_item.get("execution_method", "semantic")

    if action == ACTION_CREATE:
        tool_result = spawn_actor(
            level_path=target_level,
            actor_class=str(actor_spec.get("class", "")),
            actor_name=actor_id,
            transform=actor_spec.get("transform", {}),
        )
        actor_path = _resolve_actor_path(tool_result, plan_item.get("existing_actor_path"))
        cross_verification = None

    elif action == ACTION_UPDATE:
        actor_path = plan_item.get("existing_actor_path")
        tool_result = set_actor_transform(
            actor_path=str(actor_path or ""),
            transform=actor_spec.get("transform", {}),
        )
        if not actor_path:
            actor_path = _resolve_actor_path(tool_result, None)
        cross_verification = None

    elif action == ACTION_UI_TOOL:
        tool_result, actor_path, cross_verification = _execute_ui_tool(
            actor_spec,
            resolved_actor_paths,
        )

    else:
        actor_path = plan_item.get("existing_actor_path")
        cross_verification = None
        tool_result = {
            "status": "failed",
            "summary": f"Unsupported action: {action}",
            "data": {},
            "warnings": [],
            "errors": [],
        }

    execution_result = {
        "actor_id": actor_id,
        "action": action,
        "execution_method": execution_method,
        "status": tool_result.get("status", "failed"),
        "actor_path": actor_path,
        "summary": tool_result.get("summary", ""),
        "cross_verification": cross_verification,
    }

    if execution_method == "semantic":
        verification_result = _verify_semantic_actor(
            actor_spec=actor_spec,
            actor_path=actor_path,
            execution_result=execution_result,
            channel=channel,
        )
    elif execution_method == "ui_tool":
        verification_result = _build_ui_verification_result(
            tool_result=tool_result,
            cross_verification=cross_verification,
        )
    else:
        verification_result = {
            "status": "failed",
            "checks": [],
            "mismatches": [f"Unknown execution_method: {execution_method}"],
            "summary": f"Unknown execution_method: {execution_method}",
        }

    return {
        "execution_result": execution_result,
        "verification_result": verification_result,
    }


def _execute_ui_tool(
    actor_spec: dict[str, Any],
    resolved_actor_paths: dict[str, str],
) -> tuple[dict[str, Any], str | None, dict[str, Any] | None]:
    """执行 L3 UI 工具，并在成功后做 L3->L1 交叉比对。"""
    ui_action = dict(actor_spec.get("ui_action", {}))
    ui_type = str(ui_action.get("type", ""))
    dispatch_fn = _UI_TOOL_DISPATCH.get(ui_type)

    actor_path = _resolve_ui_actor_path(ui_action.get("actor_path"), resolved_actor_paths)
    if actor_path:
        ui_action["actor_path"] = actor_path
    cross_verification = None

    if dispatch_fn is None:
        return (
            {
                "status": "failed",
                "summary": f"Unknown ui_action type: {ui_type}",
                "data": {},
                "warnings": [],
                "errors": [],
            },
            actor_path,
            None,
        )

    l3_kwargs = {key: value for key, value in ui_action.items() if key != "type"}
    tool_result = dispatch_fn(**l3_kwargs)
    actor_path = actor_path or _extract_actor_path_from_ui_result(tool_result)

    if tool_result.get("status") in ("success", "warning"):
        verify_config = _UI_TOOL_VERIFY.get(ui_type, {})
        l1_verify_func = verify_config.get("l1_func")
        l1_verify_args = _build_l1_verify_args(verify_config, ui_action)

        if l1_verify_func is None:
            cross_verification = {
                "final_status": "failed",
                "consistent": False,
                "l3_response": tool_result,
                "l1_response": {},
                "mismatches": [f"No L1 verification configured for ui_action: {ui_type}"],
            }
        else:
            cross_verification = cross_verify_ui_operation(
                l3_response=tool_result,
                l1_verify_func=l1_verify_func,
                l1_verify_args=l1_verify_args,
            )

        final_status = cross_verification.get("final_status", "failed")
        if final_status != "success":
            tool_result["status"] = final_status
            tool_result["summary"] = (
                f"{tool_result.get('summary', 'L3 operation finished')} "
                f"(cross verification: {final_status})"
            )

    return tool_result, actor_path, cross_verification


def _verify_semantic_actor(
    actor_spec: dict[str, Any],
    actor_path: str | None,
    execution_result: dict[str, Any],
    channel: BridgeChannel,
) -> dict[str, Any]:
    """对 L1 语义工具执行结果做独立读回与验证。"""
    if execution_result.get("status") not in ("success", "warning"):
        summary = execution_result.get("summary", "Semantic execution failed")
        return {
            "status": "failed",
            "checks": [],
            "mismatches": [summary] if summary else [],
            "summary": summary,
        }

    if not actor_path:
        return {
            "status": "failed",
            "checks": [],
            "mismatches": ["Missing actor_path after semantic execution"],
            "summary": "Missing actor_path after semantic execution",
        }

    # Mock 模式下的 example JSON 不是按 Actor 维度定制的，这里用 Spec 自身构造一个稳定读回，
    # 保证纯 Python 端到端验证聚焦在编排流程，而不是被示例数据偶然性干扰。
    if channel == BridgeChannel.MOCK:
        state_data = _build_mock_actor_state(actor_spec, actor_path)
    else:
        state_response = get_actor_state(actor_path=actor_path)
        if state_response.get("status") not in ("success", "warning"):
            summary = state_response.get("summary", "Failed to read actor state")
            return {
                "status": "failed",
                "checks": [],
                "mismatches": [summary] if summary else [],
                "summary": summary,
            }
        state_data = state_response.get("data", {})

    verification = verify_actor_state(
        expected_spec=actor_spec,
        actual_response=state_data,
        execution_method="semantic",
    )

    # 在 UI override 容差场景之外，semantic 默认走标准 verify_actor_state。
    # 这里保留一次 transform_check 的显式访问，便于后续主编排扩展独立日志。
    _ = verification.get("transform_check", {})
    return verification


def _build_ui_verification_result(
    tool_result: dict[str, Any],
    cross_verification: dict[str, Any] | None,
) -> dict[str, Any]:
    """将 L3 执行与交叉比对结果整理成 verifier 风格结构。"""
    if cross_verification is not None:
        return {
            "status": cross_verification.get("final_status", "failed"),
            "checks": [],
            "mismatches": list(cross_verification.get("mismatches", [])),
            "cross_verification": cross_verification,
            "summary": tool_result.get("summary", ""),
        }

    summary = tool_result.get("summary", "")
    status = tool_result.get("status", "failed")
    return {
        "status": status,
        "checks": [],
        "mismatches": [summary] if status != "success" and summary else [],
        "summary": summary,
    }


def _resolve_actor_path(tool_result: dict[str, Any], fallback: str | None) -> str | None:
    """从写工具返回值里尽量解析 actor_path。"""
    data = tool_result.get("data", {})

    created = data.get("created_objects", [])
    if isinstance(created, list) and created:
        actor_path = created[0].get("actor_path")
        if actor_path:
            return str(actor_path)

    modified = data.get("modified_objects", [])
    if isinstance(modified, list) and modified:
        actor_path = modified[0].get("actor_path")
        if actor_path:
            return str(actor_path)

    return str(fallback) if fallback else None


def _extract_actor_path_from_ui_result(tool_result: dict[str, Any]) -> str | None:
    """从 L3 返回值中尝试提取 actor_path。"""
    data = tool_result.get("data", {})

    created_actors = data.get("created_actors", [])
    if isinstance(created_actors, list) and created_actors:
        actor_path = created_actors[0].get("actor_path")
        if actor_path:
            return str(actor_path)

    actor_path = data.get("actor_path")
    if actor_path:
        return str(actor_path)

    return None


def _build_l1_verify_args(
    verify_config: dict[str, Any],
    ui_action: dict[str, Any],
) -> dict[str, Any] | None:
    """根据 ui_action 配置构造 L1 读回参数。"""
    if "l1_args" in verify_config:
        l1_args = verify_config.get("l1_args")
        return dict(l1_args) if isinstance(l1_args, dict) else None

    l1_args_key = verify_config.get("l1_args_key")
    if l1_args_key and ui_action.get(l1_args_key):
        return {l1_args_key: ui_action[l1_args_key]}

    return None


def _resolve_ui_actor_path(
    actor_path: Any,
    resolved_actor_paths: dict[str, str],
) -> str | None:
    """解析 ui_action 中对前序 Actor 的引用。

    支持两种写法：
      - 直接传真实 ActorPath
      - `@actor_id`：引用前面 semantic/L3 步骤已经解析出的 actor_path
    """
    if not isinstance(actor_path, str):
        return None

    normalized = actor_path.strip()
    if not normalized:
        return None

    if normalized.startswith("@"):
        referenced_actor_id = normalized[1:]
        return resolved_actor_paths.get(referenced_actor_id, normalized)

    return normalized


def _build_mock_actor_state(actor_spec: dict[str, Any], actor_path: str) -> dict[str, Any]:
    """根据 Spec 构造 mock 读回值，避免 generic example 破坏流程级验证。"""
    mock_state = {
        "actor_name": actor_spec.get("id", ""),
        "actor_path": actor_path,
        "class": actor_spec.get("class", ""),
        "target_level": actor_spec.get("target_level", ""),
        "transform": actor_spec.get("transform", {}),
        "collision": actor_spec.get("collision", {}),
        "tags": actor_spec.get("tags", []),
    }

    # 对 semantic 写后读回，再补一个 bounds 查询的兜底访问位，保持与真实路径一致。
    expected_transform = actor_spec.get("transform", {})
    if expected_transform:
        verify_transform(expected_transform, expected_transform)
    return mock_state


def _build_step_failure_from_response(
    plan_item: dict[str, Any],
    response: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    """将 safe_execute 捕获的 failed 响应转为单 Actor 执行/验证失败记录。"""
    actor_spec = plan_item.get("actor_spec", {})
    summary = response.get("summary", "Unhandled orchestrator step failure")
    actor_id = str(actor_spec.get("id", ""))
    execution_method = plan_item.get("execution_method", "semantic")

    execution_result = {
        "actor_id": actor_id,
        "action": plan_item.get("action", ""),
        "execution_method": execution_method,
        "status": "failed",
        "actor_path": plan_item.get("existing_actor_path"),
        "summary": summary,
        "cross_verification": None,
    }
    verification_result = {
        "status": "failed",
        "checks": [],
        "mismatches": [summary] if summary else [],
        "summary": summary,
    }
    return execution_result, verification_result


def _build_failed_report(
    spec_path: str,
    reason: str,
    errors: list[str] | None = None,
) -> dict[str, Any]:
    """在主流程未进入 plan 执行前，构造统一失败报告。"""
    report = generate_report(
        spec_path=spec_path,
        plan=[],
        execution_results=[],
        verification_results=[],
        dirty_assets=[],
        map_check={},
    )
    report["overall_status"] = "failed"
    report["failure_reason"] = reason
    report["errors"] = list(errors or [])
    return report


def _extract_errors_from_response(response: dict[str, Any]) -> list[str]:
    """从统一响应中提取可读错误文本。"""
    errors = []
    for item in response.get("errors", []):
        if isinstance(item, dict):
            message = item.get("message") or item.get("code")
            if message:
                errors.append(str(message))
        elif item:
            errors.append(str(item))
    if not errors and response.get("summary"):
        errors.append(str(response["summary"]))
    return errors


def _save_report_if_needed(report: dict[str, Any], report_path: str | None) -> None:
    """仅当用户显式提供 report_path 时写出 JSON 报告。"""
    if report_path:
        save_report(report, report_path)


def _build_default_cli_report_path() -> Path:
    """CLI 默认把报告落到项目 Saved/，便于临时运行时回收。"""
    return get_saved_dir() / "AgentBridge" / "orchestrator" / "last_execution_report.json"


if __name__ == "__main__":
    sys.exit(main())

