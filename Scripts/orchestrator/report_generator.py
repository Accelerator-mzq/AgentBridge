"""汇总执行计划、执行结果与验证结果，生成结构化报告。"""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
import sys
from typing import Any

_THIS_DIR = Path(__file__).resolve().parent
_BRIDGE_DIR = _THIS_DIR.parent / "bridge"

if str(_BRIDGE_DIR) not in sys.path:
    sys.path.insert(0, str(_BRIDGE_DIR))

from project_config import get_reports_dir


def generate_report(
    spec_path: str,
    plan: list[dict[str, Any]],
    execution_results: list[dict[str, Any]],
    verification_results: list[dict[str, Any]],
    dirty_assets: list[str] | None = None,
    map_check: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """根据计划、执行结果和验证结果生成最终结构化报告。"""
    dirty_asset_list = list(dirty_assets or [])
    normalized_map_check = _normalize_map_check(map_check)
    actor_entries: list[dict[str, Any]] = []

    execution_method_summary = {
        "semantic": 0,
        "ui_tool": 0,
    }

    for index, plan_item in enumerate(plan):
        actor_spec = plan_item.get("actor_spec", {})
        actor_id = str(actor_spec.get("id", ""))
        action = plan_item.get("action", "SKIP")
        execution_method = plan_item.get("execution_method", "semantic")

        if execution_method not in execution_method_summary:
            execution_method_summary[execution_method] = 0
        execution_method_summary[execution_method] += 1

        execution_result = execution_results[index] if index < len(execution_results) else {}
        verification_result = verification_results[index] if index < len(verification_results) else {}

        exec_status = str(execution_result.get("status", "skipped"))
        verify_status = str(verification_result.get("status", "skipped"))
        mismatches = _collect_mismatches(execution_result, verification_result)
        cross_verification = _extract_cross_verification(
            execution_method=execution_method,
            execution_result=execution_result,
            verification_result=verification_result,
        )

        actor_entry = {
            "actor_id": actor_id,
            "action": action,
            "execution_method": execution_method,
            "exec_status": exec_status,
            "verify_status": verify_status,
            "actor_path": execution_result.get("actor_path", plan_item.get("existing_actor_path")),
            "mismatches": mismatches,
            "cross_verification": cross_verification,
        }
        actor_entry["final_status"] = _compute_actor_final_status(actor_entry)
        actor_entries.append(actor_entry)

    summary = _build_summary(actor_entries, execution_method_summary)
    overall_status = _compute_overall_status(actor_entries)

    return {
        "spec_path": spec_path,
        "timestamp": _utc_timestamp(),
        "overall_status": overall_status,
        "summary": summary,
        "actors": actor_entries,
        "dirty_assets": dirty_asset_list,
        "map_check": normalized_map_check,
    }


def save_report(report: dict[str, Any], output_path: str) -> None:
    """将报告写入 JSON 文件。"""
    path = Path(output_path)
    if not path.is_absolute():
        path = get_reports_dir() / path
    path.parent.mkdir(parents=True, exist_ok=True)

    with path.open("w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, ensure_ascii=False)


def format_summary(report: dict[str, Any]) -> str:
    """返回控制台友好的单块文本摘要。"""
    summary = report.get("summary", {})
    map_check = report.get("map_check", {})

    return "\n".join(
        [
            "=== AGENT UE5 Execution Report ===",
            f"Spec: {report.get('spec_path', '')}",
            f"Overall: {str(report.get('overall_status', '')).upper()}",
            "Actors: "
            f"{summary.get('total', 0)} total / "
            f"{summary.get('passed', 0)} passed / "
            f"{summary.get('mismatched', 0)} mismatch / "
            f"{summary.get('failed', 0)} failed / "
            f"{summary.get('skipped', 0)} skipped",
            f"Dirty Assets: {len(report.get('dirty_assets', []))}",
            f"Map Errors: {map_check.get('map_errors', 0)} / Warnings: {map_check.get('map_warnings', 0)}",
        ]
    )


def print_summary(report: dict[str, Any]) -> None:
    """在控制台打印人类可读摘要。"""
    print(format_summary(report))


def _build_summary(
    actor_entries: list[dict[str, Any]],
    execution_method_summary: dict[str, int],
) -> dict[str, Any]:
    """汇总总数、状态分布和执行方式分布。"""
    total = len(actor_entries)
    passed = sum(1 for actor in actor_entries if actor.get("final_status") == "success")
    mismatched = sum(1 for actor in actor_entries if actor.get("final_status") == "mismatch")
    failed = sum(1 for actor in actor_entries if actor.get("final_status") == "failed")
    skipped = sum(1 for actor in actor_entries if actor.get("final_status") == "skipped")

    return {
        "total": total,
        "total_actors": total,
        "passed": passed,
        "mismatched": mismatched,
        "failed": failed,
        "skipped": skipped,
        "execution_methods": execution_method_summary,
    }


def _compute_overall_status(actor_entries: list[dict[str, Any]]) -> str:
    """按 failed > mismatch > success/skipped 的优先级汇总 overall_status。"""
    final_statuses = [actor.get("final_status", "skipped") for actor in actor_entries]

    if any(status == "failed" for status in final_statuses):
        return "failed"
    if any(status == "mismatch" for status in final_statuses):
        return "mismatch"
    return "success"


def _compute_actor_final_status(actor_entry: dict[str, Any]) -> str:
    """根据 exec_status 与 verify_status 计算单 Actor 最终状态。"""
    exec_status = str(actor_entry.get("exec_status", "skipped"))
    verify_status = str(actor_entry.get("verify_status", "skipped"))
    statuses = {exec_status, verify_status}

    if "failed" in statuses:
        return "failed"
    if "mismatch" in statuses:
        return "mismatch"
    if exec_status == "skipped" or verify_status == "skipped":
        return "skipped"
    return "success"


def _collect_mismatches(
    execution_result: dict[str, Any],
    verification_result: dict[str, Any],
) -> list[str]:
    """优先汇总 verifier 产生的 mismatch 信息，并兼容执行阶段错误。"""
    mismatches = list(verification_result.get("mismatches") or [])

    if not mismatches and verification_result.get("status") == "failed":
        summary = verification_result.get("summary")
        if summary:
            mismatches.append(str(summary))

    if execution_result.get("status") in {"failed", "mismatch"}:
        summary = execution_result.get("summary")
        if summary and summary not in mismatches:
            mismatches.append(str(summary))

    return mismatches


def _extract_cross_verification(
    execution_method: str,
    execution_result: dict[str, Any],
    verification_result: dict[str, Any],
) -> dict[str, Any] | None:
    """仅对 L3 条目保留交叉验证结果。"""
    if execution_method != "ui_tool":
        return None

    if isinstance(verification_result.get("cross_verification"), dict):
        return verification_result["cross_verification"]
    if isinstance(execution_result.get("cross_verification"), dict):
        return execution_result["cross_verification"]
    return None


def _normalize_map_check(map_check: dict[str, Any] | None) -> dict[str, int]:
    """兼容桥接层返回格式和扁平格式，统一输出错误/警告计数。"""
    if not isinstance(map_check, dict):
        return {
            "map_errors": 0,
            "map_warnings": 0,
        }

    if isinstance(map_check.get("data"), dict):
        data = map_check["data"]
        return {
            "map_errors": _count_map_items(data.get("map_errors")),
            "map_warnings": _count_map_items(data.get("map_warnings")),
        }

    return {
        "map_errors": _count_map_items(map_check.get("map_errors")),
        "map_warnings": _count_map_items(map_check.get("map_warnings")),
    }


def _count_map_items(value: Any) -> int:
    """支持 int 或 list 两种 map check 表达方式。"""
    if isinstance(value, int):
        return value
    if isinstance(value, list):
        return len(value)
    return 0


def _utc_timestamp() -> str:
    """输出 ISO 8601 UTC 时间戳。"""
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
