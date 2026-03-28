"""
ui_tools.py
===========
AGENT + UE5 可操作層 — L3 UI 工具（Automation Driver 执行后端）。

工具优先级：L1 语义工具 > L2 编辑器服务工具 > L3 UI 工具。
仅当 L1 语义工具无法覆盖时使用。

三通道实现：
  通道 C: C++ Plugin（推荐，AgentBridgeSubsystem 的 UITool 接口）
  通道 A/B: 不适用（L3 依赖 Automation Driver，只能在 Editor 进程内执行）
  Mock: 返回模拟成功响应

每次 L3 操作后，必须通过 L1 工具做独立验证。
L3 返回值与 L1 验证返回值做交叉比对——两者一致才判定 success。

使用判定条件（全部满足才允许使用）：
  1. L1 无对应 API
  2. 操作可结构化（路径/名称，非坐标）
  3. 结果可验证（有对应的 L1 读回接口）
  4. 操作可逆或低风险
  5. Spec 中显式标注 execution_method: ui_tool
  6. 已封装为 AgentBridge 接口
"""

from __future__ import annotations
import time
from typing import Optional, Dict, Any, List

from bridge_core import (
    get_channel, BridgeChannel,
    make_response, make_error, safe_execute,
    validate_required_string, call_cpp_plugin,
)
from query_tools import list_level_actors


# ============================================================
# L3 → C++ Plugin 函数名映射
# ============================================================

_CPP_UI_TOOL_MAP = {
    "click_detail_panel_button": "ClickDetailPanelButton",
    "type_in_detail_panel_field": "TypeInDetailPanelField",
    "drag_asset_to_viewport": "DragAssetToViewport",
    "is_automation_driver_available": "IsAutomationDriverAvailable",
    "start_ui_operation": "StartUIOperation",
    "query_ui_operation": "QueryUIOperation",
}

_ASYNC_UI_POLL_INTERVAL_SECONDS = 0.25
_ASYNC_UI_TIMEOUT_GRACE_SECONDS = 5.0


def _channel_value(channel) -> str:
    """统一提取通道值，兼容重复导入时的 Enum 身份差异。"""
    return getattr(channel, "value", str(channel))


# ============================================================
# 通道分发
# ============================================================

def _dispatch_ui_tool(tool_name: str, cpp_params: Optional[Dict] = None) -> dict:
    """L3 UI 工具的通道分发。

    L3 工具依赖 Automation Driver，只能在 Editor 进程内执行。
    因此只支持通道 C（C++ Plugin）和 Mock 模式。
    通道 A/B 不适用——返回错误提示。
    """
    channel = get_channel()

    if _channel_value(channel) == BridgeChannel.MOCK.value:
        return _mock_ui_tool_response(tool_name, cpp_params)

    if _channel_value(channel) == BridgeChannel.CPP_PLUGIN.value:
        cpp_func = _CPP_UI_TOOL_MAP.get(tool_name)
        if not cpp_func:
            return make_response(
                status="failed",
                summary=f"No C++ mapping for L3 tool: {tool_name}",
                data={},
                errors=[make_error("INVALID_ARGS", f"Unknown L3 tool: {tool_name}")]
            )
        return call_cpp_plugin(cpp_func, cpp_params)

    # 通道 A / B 不支持 L3
    return make_response(
        status="failed",
        summary=f"L3 UI tools require channel CPP_PLUGIN, current: {channel.value}",
        data={"tool_layer": "L3_UITool"},
        errors=[make_error("INVALID_ARGS",
                          "L3 UI tools depend on Automation Driver and can only run "
                          "via C++ Plugin (channel CPP_PLUGIN). "
                          "Use set_channel(BridgeChannel.CPP_PLUGIN) first.")]
    )


def _mock_ui_tool_response(tool_name: str, params: Optional[Dict] = None) -> dict:
    """Mock 模式下的 L3 工具返回值。"""
    data = {
        "operation": tool_name,
        "executed": True,
        "ui_idle_after": True,
        "duration_seconds": 0.5,
        "tool_layer": "L3_UITool",
    }
    if params:
        data.update({k: v for k, v in params.items() if isinstance(v, (str, int, float, bool))})
    return make_response(
        status="success",
        summary=f"Mock L3: {tool_name}",
        data=data,
    )


def _run_async_ui_operation(
    operation_type: str,
    target: str,
    actor_path: str = "",
    value: str = "",
    timeout_seconds: float = 10.0,
    dry_run: bool = False,
) -> dict:
    """统一封装异步 UI 原型的启动与轮询。

    设计目的：
      1. 让上层继续使用同步风格的 Python 函数签名
      2. 实际执行改走 start_ui_operation / query_ui_operation
      3. 避开旧的 RC 同步链直接等待 UI 操作完成
    """
    channel = get_channel()

    if _channel_value(channel) == BridgeChannel.MOCK.value:
        return _mock_ui_tool_response(operation_type, {
            "actor_path": actor_path,
            "target": target,
            "value": value,
            "timeout_seconds": timeout_seconds,
            "operation_state": "success",
            "execution_backend": "mock_async_ui_operation",
        })

    start_response = start_ui_operation(
        operation_type=operation_type,
        actor_path=actor_path,
        target=target,
        value=value,
        timeout_seconds=timeout_seconds,
        dry_run=dry_run,
    )
    if start_response.get("status") not in ("success", "warning"):
        return start_response

    if dry_run:
        return start_response

    operation_id = start_response.get("data", {}).get("operation_id")
    if not operation_id:
        return make_response(
            status="failed",
            summary="Async UI operation did not return operation_id",
            data={"operation_type": operation_type, "tool_layer": "L3_UITool_AsyncPrototype"},
            errors=[make_error("TOOL_EXECUTION_FAILED", "Missing operation_id in start_ui_operation response")],
        )

    deadline = time.time() + max(timeout_seconds + _ASYNC_UI_TIMEOUT_GRACE_SECONDS, 1.0)
    last_query = start_response

    while time.time() < deadline:
        query_response = query_ui_operation(operation_id)
        last_query = query_response

        query_state = query_response.get("data", {}).get("operation_state")
        if query_state in ("success", "failed"):
            return query_response

        if query_response.get("status") not in ("success", "warning"):
            return query_response

        time.sleep(_ASYNC_UI_POLL_INTERVAL_SECONDS)

    timeout_data = dict(last_query.get("data", {}))
    timeout_data.setdefault("operation_id", operation_id)
    timeout_data.setdefault("operation_type", operation_type)
    timeout_data["operation_state"] = timeout_data.get("operation_state", "running")

    return make_response(
        status="failed",
        summary=f"Async UI operation polling timed out: {operation_type}",
        data=timeout_data,
        errors=[make_error("TIMEOUT", f"query_ui_operation timed out after {timeout_seconds + _ASYNC_UI_TIMEOUT_GRACE_SECONDS:.2f}s")],
    )


# ============================================================
# L3 接口
# ============================================================

def is_automation_driver_available() -> bool:
    """检查 Automation Driver 是否可用。

    返回 bool，不是 dict（这是查询性质的辅助函数）。
    Mock 模式下返回 True。
    """
    channel = get_channel()
    if _channel_value(channel) == BridgeChannel.MOCK.value:
        return True
    if _channel_value(channel) == BridgeChannel.CPP_PLUGIN.value:
        resp = call_cpp_plugin("IsAutomationDriverAvailable")
        # RC API 返回 bool 类型的 ReturnValue
        if isinstance(resp, bool):
            return resp
        if isinstance(resp, dict) and "ReturnValue" in resp:
            return bool(resp["ReturnValue"])
        return False
    return False


def click_detail_panel_button(
    actor_path: str,
    button_label: str,
    dry_run: bool = False,
) -> dict:
    """在 Actor 的 Detail Panel 中点击按钮。

    L3 UI 工具——仅当无直接 API 时使用。
    调用后必须通过 L1 get_actor_state 或 get_component_state 验证状态变更。

    参数：
        actor_path: 目标 Actor 完整路径
        button_label: 按钮显示文本（精确匹配）
        dry_run: 仅校验参数和 Driver 可用性

    返回：
        FBridgeResponse 格式（tool_layer: L3_UITool_AsyncPrototype）。
        当前默认实现改走 start_ui_operation / query_ui_operation，
        底层点击由异步原型决定具体执行后端。
    """
    err = validate_required_string(actor_path, "actor_path")
    if err:
        return err
    err = validate_required_string(button_label, "button_label")
    if err:
        return err

    return _run_async_ui_operation(
        operation_type="click_detail_panel_button",
        actor_path=actor_path,
        target=button_label,
        dry_run=dry_run,
    )


def type_in_detail_panel_field(
    actor_path: str,
    property_path: str,
    value: str,
    dry_run: bool = False,
) -> dict:
    """在 Detail Panel 的属性输入框中输入值。

    L3 UI 工具——用于非反射属性或需要 UI 触发的属性。
    调用后必须通过 L1 get_actor_state 验证属性值变更。

    参数：
        actor_path: 目标 Actor 完整路径
        property_path: 属性路径（如 "StaticMeshComponent.StaticMesh"）
        value: 要输入的值（字符串形式）
        dry_run: 仅校验参数和 Driver 可用性
    """
    err = validate_required_string(actor_path, "actor_path")
    if err:
        return err
    err = validate_required_string(property_path, "property_path")
    if err:
        return err
    err = validate_required_string(value, "value")
    if err:
        return err

    # 统一走异步任务壳，避免在 RC 同步链里直接等待完整的 Detail Panel 输入交互。
    result = _run_async_ui_operation(
        operation_type="type_in_detail_panel_field",
        actor_path=actor_path,
        target=property_path,
        value=value,
        dry_run=dry_run,
    )

    # 显式补齐字段输入语义，避免后续交叉比对把 target/value 误判成其它 UI 工具。
    data = result.setdefault("data", {})
    data["property_path"] = property_path
    data["typed_value"] = value
    return result


def drag_asset_to_viewport(
    asset_path: str,
    drop_location: list,
    dry_run: bool = False,
) -> dict:
    """将 Content Browser 中的资产拖拽到 Viewport 指定位置。

    L3 UI 工具——走 Editor 原生 OnDropped 流程（触发自动碰撞/命名/贴地）。
    与 L1 spawn_actor 的区别：spawn_actor 不触发这些 Editor 默认行为。
    调用后必须通过 L1 list_level_actors + get_actor_state 验证。

    参数：
        asset_path: 资产路径（如 /Game/Meshes/SM_Chair）
        drop_location: 世界坐标 [x, y, z]
        dry_run: 仅校验参数和 Driver 可用性
    """
    err = validate_required_string(asset_path, "asset_path")
    if err:
        return err

    if not isinstance(drop_location, (list, tuple)) or len(drop_location) != 3:
        return make_response(
            status="validation_error",
            summary="drop_location must be [x, y, z]",
            data={},
            errors=[make_error("INVALID_ARGS", "drop_location must be a list of 3 numbers")]
        )

    channel = get_channel()
    if _channel_value(channel) == BridgeChannel.MOCK.value:
        response = _mock_ui_tool_response("drag_asset_to_viewport", {
            "asset_path": asset_path,
            "drop_location": list(drop_location),
        })
        response.setdefault("data", {})["drop_location"] = list(drop_location)
        return response

    # 拖拽也统一纳入异步任务壳，避免在 RC 同步链里等待整段 Slate/Viewport 交互结束。
    # 同时补一层最基本的 L1 读回：比较前后 actor 列表，避免“UI 执行成功但实际未落地”被误报为 success。
    before_response = list_level_actors()
    before_actors = before_response.get("data", {}).get("actors", []) if before_response.get("status") in ("success", "warning") else []
    before_paths = {actor.get("actor_path", "") for actor in before_actors if isinstance(actor, dict)}

    drop_location_value = ",".join(str(v) for v in drop_location)
    result = _run_async_ui_operation(
        operation_type="drag_asset_to_viewport",
        actor_path="",
        target=asset_path,
        value=drop_location_value,
        dry_run=dry_run,
    )

    if dry_run or result.get("status") not in ("success", "warning"):
        return result

    after_response = list_level_actors()
    after_actors = after_response.get("data", {}).get("actors", []) if after_response.get("status") in ("success", "warning") else []
    after_paths = {actor.get("actor_path", "") for actor in after_actors if isinstance(actor, dict)}

    new_actor_paths = sorted(path for path in (after_paths - before_paths) if path)
    new_actors = [actor for actor in after_actors if actor.get("actor_path", "") in new_actor_paths]

    data = result.setdefault("data", {})
    data["actors_before"] = len(before_paths)
    data["actors_after"] = len(after_paths)
    data["actors_created"] = len(new_actor_paths)
    data["created_actors"] = new_actors
    data["drop_location"] = list(drop_location)

    if len(new_actor_paths) == 0:
        result["status"] = "mismatch"
        result["summary"] = (
            f"{result.get('summary', 'Async drag finished')} "
            f"(L1 readback: no new actor detected)"
        )

    return result


def start_ui_operation(
    operation_type: str,
    actor_path: str,
    target: str,
    value: str = "",
    timeout_seconds: float = 10.0,
    dry_run: bool = False,
) -> dict:
    """启动异步 UI 操作原型。

    当前最小支持范围：
      - click_detail_panel_button
      - type_in_detail_panel_field
      - drag_asset_to_viewport

    参数映射：
      - click_detail_panel_button: actor_path=ActorPath, target=ButtonLabel
      - type_in_detail_panel_field: actor_path=ActorPath, target=PropertyPath, value=输入值
      - drag_asset_to_viewport: actor_path 可为空, target=AssetPath, value='X,Y,Z'

    返回：
      FBridgeResponse，data 内含 operation_id / operation_state。
    """
    err = validate_required_string(operation_type, "operation_type")
    if err:
        return err
    normalized_operation = operation_type.strip().lower()
    if normalized_operation in ("click_detail_panel_button", "type_in_detail_panel_field"):
        err = validate_required_string(actor_path, "actor_path")
        if err:
            return err
    err = validate_required_string(target, "target")
    if err:
        return err
    if normalized_operation in ("type_in_detail_panel_field", "drag_asset_to_viewport"):
        err = validate_required_string(value, "value")
        if err:
            return err

    return _dispatch_ui_tool("start_ui_operation", {
        "OperationType": operation_type,
        "ActorPath": actor_path,
        "Target": target,
        "Value": value,
        "TimeoutSeconds": timeout_seconds,
        "bDryRun": dry_run,
    })


def query_ui_operation(operation_id: str) -> dict:
    """查询异步 UI 操作原型的当前状态。"""
    err = validate_required_string(operation_id, "operation_id")
    if err:
        return err

    return _dispatch_ui_tool("query_ui_operation", {
        "OperationId": operation_id,
    })


# ============================================================
# L3 → L1 交叉比对辅助（Python 端）
# ============================================================

def cross_verify_ui_operation(
    l3_response: dict,
    l1_verify_func,
    l1_verify_args: Optional[dict] = None,
) -> dict:
    """L3→L1 交叉比对：拿 L3 返回值与 L1 独立读回做比对。

    参数：
        l3_response: L3 UI 工具的返回值（dict）
        l1_verify_func: L1 验证函数（callable，如 query_tools.get_actor_state）
        l1_verify_args: L1 验证函数的参数（dict，如 {"actor_path": "xxx"}）

    返回：
        {
            "final_status": "success" / "mismatch" / "failed",
            "consistent": bool,
            "l3_response": {...},
            "l1_response": {...},
            "mismatches": ["field: L3=x, L1=y", ...]
        }
    """
    result = {
        "final_status": "failed",
        "consistent": False,
        "l3_response": l3_response,
        "l1_response": {},
        "mismatches": [],
    }

    # L3 本身失败
    if l3_response.get("status") not in ("success", "warning"):
        result["mismatches"].append(f"L3 operation failed: {l3_response.get('summary', '')}")
        return result

    # 执行 L1 独立读回
    try:
        if l1_verify_args:
            l1_response = l1_verify_func(**l1_verify_args)
        else:
            l1_response = l1_verify_func()
    except Exception as e:
        result["mismatches"].append(f"L1 verification raised exception: {e}")
        return result

    result["l1_response"] = l1_response

    # L1 失败
    if l1_response.get("status") not in ("success", "warning"):
        result["mismatches"].append(f"L1 verification failed: {l1_response.get('summary', '')}")
        return result

    # 两者都成功——做字段级比对
    l3_data = l3_response.get("data", {})
    l1_data = l1_response.get("data", {})
    mismatches = []

    # DragAssetToViewport 特殊比对：
    # 检查 L3 声称的 created_actors 是否在 L1 的 actor 列表中。
    if "actors_created" in l3_data and l3_data["actors_created"] > 0:
        l3_created = l3_data.get("created_actors", [])
        l1_actors = l1_data.get("actors", [])
        l1_paths = {a.get("actor_path", "") for a in l1_actors} if isinstance(l1_actors, list) else set()

        for ca in l3_created:
            ca_path = ca.get("actor_path", "") if isinstance(ca, dict) else ""
            if ca_path and ca_path not in l1_paths:
                mismatches.append(f"L3 created actor '{ca_path}' not found in L1 actor list")

    # TypeInDetailPanelField 特殊比对：
    # 用 L1 get_actor_state 读回后的真实属性值，对照 L3 typed_value 做二次确认。
    is_type_operation = (
        l3_data.get("operation_type") == "type_in_detail_panel_field"
        or l3_data.get("operation") == "TypeInDetailPanelField"
        or ("property_path" in l3_data and "typed_value" in l3_data)
    )
    if is_type_operation:
        property_path = l3_data.get("property_path") or l3_data.get("target", "")
        typed_value = l3_data.get("typed_value", l3_data.get("value"))
        actual_value = _extract_property_value_from_actor_state(l1_data, property_path)

        # Detail Panel 数值提交偶发会比 query_ui_operation 的 success 晚一个短 Tick。
        # 这里补一个极短的 L1 重试窗口，优先收敛真正的 UI 结果，再决定是否 mismatch。
        if actual_value is None or not _values_match_for_ui_property(typed_value, actual_value):
            retried_l1_response, retried_actual_value = _retry_ui_property_readback(
                l1_verify_func=l1_verify_func,
                l1_verify_args=l1_verify_args,
                property_path=property_path,
                expected_value=typed_value,
                initial_response=l1_response,
                initial_value=actual_value,
            )
            l1_response = retried_l1_response
            l1_data = l1_response.get("data", {}) if isinstance(l1_response, dict) else {}
            actual_value = retried_actual_value
            result["l1_response"] = l1_response

        if actual_value is None:
            mismatches.append(f"L1 readback could not resolve property path: {property_path}")
        elif not _values_match_for_ui_property(typed_value, actual_value):
            mismatches.append(
                f"Property mismatch for '{property_path}': L3 typed={typed_value}, L1 actual={actual_value}"
            )

    result["mismatches"] = mismatches
    result["consistent"] = len(mismatches) == 0
    result["final_status"] = "success" if len(mismatches) == 0 else "mismatch"

    return result


def _extract_property_value_from_actor_state(actor_state: dict, property_path: str):
    """从 get_actor_state 返回值中解析少量高价值 Detail Panel 属性。"""
    if not isinstance(actor_state, dict) or not isinstance(property_path, str):
        return None

    normalized = property_path.strip()
    transform = actor_state.get("transform", {})
    if not isinstance(transform, dict):
        transform = {}

    vector_property_map = {
        "RelativeLocation": ("location", {"X": 0, "Y": 1, "Z": 2}),
        "Location": ("location", {"X": 0, "Y": 1, "Z": 2}),
        "RelativeScale3D": ("relative_scale3d", {"X": 0, "Y": 1, "Z": 2}),
        "Scale3D": ("relative_scale3d", {"X": 0, "Y": 1, "Z": 2}),
        "Rotation": ("rotation", {"Pitch": 0, "Yaw": 1, "Roll": 2}),
        "RelativeRotation": ("rotation", {"Pitch": 0, "Yaw": 1, "Roll": 2}),
    }

    segments = [segment for segment in normalized.split(".") if segment]
    if len(segments) == 2:
        parent_key, axis_key = segments
        transform_key, axis_map = vector_property_map.get(parent_key, (None, None))
        if transform_key and axis_map and axis_key in axis_map:
            values = transform.get(transform_key, [])
            if isinstance(values, list) and len(values) >= 3:
                return values[axis_map[axis_key]]

    return None


def _values_match_for_ui_property(expected_value, actual_value) -> bool:
    """对 UI 属性输入值做宽松但可审计的相等判定。"""
    try:
        return abs(float(expected_value) - float(actual_value)) <= 0.01
    except (TypeError, ValueError):
        return str(expected_value) == str(actual_value)


def _retry_ui_property_readback(
    l1_verify_func,
    l1_verify_args: Optional[dict],
    property_path: str,
    expected_value,
    initial_response: dict,
    initial_value,
):
    """对 Detail Panel 字段输入补一个很短的读回稳定窗口。"""
    latest_response = initial_response
    latest_value = initial_value

    for _ in range(6):
        time.sleep(0.25)
        if l1_verify_args:
            latest_response = l1_verify_func(**l1_verify_args)
        else:
            latest_response = l1_verify_func()

        if latest_response.get("status") not in ("success", "warning"):
            continue

        latest_value = _extract_property_value_from_actor_state(
            latest_response.get("data", {}),
            property_path,
        )
        if latest_value is not None and _values_match_for_ui_property(expected_value, latest_value):
            break

    return latest_response, latest_value

