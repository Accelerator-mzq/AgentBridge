"""
bridge_core.py
==============
AGENT + UE5 可操作层 — Bridge 封装层核心模块。

职责：
  - 统一响应构造（make_response / make_error）
  - 通道切换（Python Editor Scripting / Remote Control API / C++ Plugin / Mock）
  - 参数校验（validate_transform / validate_required_string）
  - 安全执行包装（safe_execute）
  - Mock 模式（开发调试用）

三层受控工具体系：
  L1 语义工具（query_tools.py / write_tools.py）→ 默认主干
  L2 编辑器服务工具（uat_runner.py 等）→ 工程服务
  L3 UI 工具（ui_tools.py）→ Automation Driver 执行后端，仅 fallback

核心定位：
  Bridge 封装层的全部执行能力来自 UE5 官方 API。
  本模块不创造引擎 API——它提供编排层的统一封装和护栏。

UE5 官方能力依赖：
  - 通道 A: Python Editor Scripting（unreal 模块，进程内调用）
  - 通道 B: Remote Control API（HTTP REST，远程调用）
  - 通道 C: C++ Plugin（AgentBridgeSubsystem，推荐）
"""

from __future__ import annotations

import json
from enum import Enum
from pathlib import Path
from typing import Any, Dict, List, Optional


# ============================================================
# 通道配置
# ============================================================

class BridgeChannel(Enum):
    """Bridge 执行通道。

    PYTHON_EDITOR:  通道 A — unreal 模块进程内调用（UE5 Python Editor Scripting）
    REMOTE_CONTROL: 通道 B — HTTP Remote Control API（UE5 Remote Control Plugin）
    CPP_PLUGIN:     通道 C — 通过 RC API 调用 C++ AgentBridge Plugin 的 Subsystem 接口
                    与通道 B 的区别：通道 B 直接调用 UE5 原生 API，
                    通道 C 调用 AgentBridge Plugin 封装后的统一接口（含参数校验 + 统一响应）
    MOCK:           Mock 模式 — 返回 agent_ue5/schemas/examples/ 中的 example JSON（开发调试用）

    推荐使用优先级：
      C++ Plugin 已安装时 → CPP_PLUGIN（最完整：参数校验 + Transaction + 统一响应全在 C++ 端）
      快速原型 / 无 Plugin → PYTHON_EDITOR 或 REMOTE_CONTROL
      开发调试 → MOCK
    """
    PYTHON_EDITOR = "python_editor"
    REMOTE_CONTROL = "remote_control"
    CPP_PLUGIN = "cpp_plugin"
    MOCK = "mock"


# 默认通道 — 开发时用 MOCK，接入 C++ Plugin 后切换到 CPP_PLUGIN
ACTIVE_CHANNEL = BridgeChannel.MOCK


def set_channel(channel: BridgeChannel):
    """切换执行通道。"""
    global ACTIVE_CHANNEL
    ACTIVE_CHANNEL = channel


def get_channel() -> BridgeChannel:
    """获取当前执行通道。"""
    return ACTIVE_CHANNEL


# ============================================================
# 统一响应构造
# ============================================================

VALID_STATUSES = {"success", "warning", "failed", "mismatch", "validation_error"}


def make_response(
    status: str,
    summary: str,
    data: Dict[str, Any],
    warnings: Optional[List[str]] = None,
    errors: Optional[List] = None,
) -> dict:
    """
    构造统一响应外壳。

    所有 Bridge 函数必须通过此函数返回，确保与 Schema 外壳一致：
    {status, summary, data, warnings, errors}

    status 枚举（与 agent_ue5/schemas/common/primitives.schema.json 中定义一致）：
      success / warning / failed / mismatch / validation_error
    """
    assert status in VALID_STATUSES, (
        f"Invalid status: {status}. Must be one of {VALID_STATUSES}"
    )
    return {
        "status": status,
        "summary": summary,
        "data": data,
        "warnings": warnings or [],
        "errors": errors or [],
    }


def make_error(code: str, message: str, details: Optional[dict] = None) -> dict:
    """
    构造结构化错误对象。

    错误码定义见 Docs/tool_contract_v0_1.md 第 2.4 节：
    INVALID_ARGS / ACTOR_NOT_FOUND / ASSET_NOT_FOUND / CLASS_NOT_FOUND /
    EDITOR_NOT_READY / TOOL_EXECUTION_FAILED / READBACK_FAILED 等
    """
    err = {"code": code, "message": message}
    if details:
        err["details"] = details
    return err


# ============================================================
# 安全执行包装
# ============================================================

def safe_execute(func, *args, timeout: float = 0, **kwargs) -> dict:
    """
    安全执行包装器。捕获 UE5 API 异常，转为统一 failed 响应。
    确保 Bridge 函数永远不会抛出未处理异常。

    timeout: 超时秒数（0=不限时）。借鉴 Gauntlet 的 MaxDuration 模式。
    """
    import signal

    def _timeout_handler(signum, frame):
        raise TimeoutError(f"Execution timed out after {timeout}s")

    try:
        if timeout > 0:
            try:
                old_handler = signal.signal(signal.SIGALRM, _timeout_handler)
                signal.alarm(int(timeout))
            except (AttributeError, ValueError):
                pass  # Windows 不支持 SIGALRM，跳过超时机制

        result = func(*args, **kwargs)

        if timeout > 0:
            try:
                signal.alarm(0)  # 取消超时
            except (AttributeError, ValueError):
                pass

        return result
    except TimeoutError as e:
        return make_response(
            status="failed",
            summary=f"Execution timed out",
            data={},
            errors=[make_error("TIMEOUT", str(e))],
        )
    except Exception as e:
        return make_response(
            status="failed",
            summary=f"Execution failed: {type(e).__name__}",
            data={},
            errors=[make_error("TOOL_EXECUTION_FAILED", str(e))],
        )


# ============================================================
# 通道 C: C++ Plugin 调用辅助
# ============================================================

def call_cpp_plugin(function_name: str, parameters: Optional[Dict[str, Any]] = None) -> dict:
    """
    通过 Remote Control API 调用 C++ AgentBridge Plugin 的 Subsystem 接口。
    这是通道 C 的 Python 客户端入口。

    与通道 B（直接调用 UE5 原生 API）的区别：
    通道 C 调用的是 AgentBridge Plugin 已封装好的 BlueprintCallable 函数，
    C++ 端已完成参数校验、Transaction 管理、写后读回。
    Python 端收到的是完整的 FBridgeResponse JSON。

    参数：
        function_name: Subsystem 函数名（如 "SpawnActor" / "GetActorState"）
        parameters: 函数参数（dict，传递给 RC API）

    返回：
        FBridgeResponse 的 JSON 解析结果（与 make_response 格式一致）
    """
    try:
        from .remote_control_client import call_function, RemoteControlError
    except ImportError:
        from remote_control_client import call_function, RemoteControlError

    # AgentBridgeSubsystem 的默认对象路径
    subsystem_path = "/Script/AgentBridge.Default__AgentBridgeSubsystem"

    try:
        raw_response = call_function(
            object_path=subsystem_path,
            function_name=function_name,
            parameters=parameters,
            generate_transaction=False,  # Transaction 由 C++ 端的 FScopedTransaction 管理
        )

        # C++ Subsystem 返回 FBridgeResponse，RC API 会将其序列化为 JSON
        # 检查是否有 ReturnValue（RC API 的标准返回格式）
        if "ReturnValue" in raw_response:
            return _normalize_cpp_plugin_return_value(raw_response["ReturnValue"])

        # 如果 RC API 直接返回了响应结构
        if "status" in raw_response:
            return _normalize_cpp_plugin_return_value(raw_response)

        # fallback：包装为 success
        return make_response(
            status="success",
            summary=f"C++ Plugin call: {function_name}",
            data=raw_response,
        )

    except RemoteControlError as e:
        return make_response(
            status="failed",
            summary=f"C++ Plugin call failed: {function_name}",
            data={},
            errors=[make_error("TOOL_EXECUTION_FAILED",
                              f"RC API error calling {function_name}: {e}")],
        )


def _normalize_cpp_plugin_return_value(value: Any) -> Any:
    """展开 C++ Plugin 经 RC API 返回的 FBridgeResponse/FJsonObjectWrapper。"""
    if not isinstance(value, dict):
        return value

    normalized = dict(value)
    data = normalized.get("data")
    if isinstance(data, dict):
        normalized["data"] = _unwrap_fjsonobjectwrapper(data)

    return normalized


def _unwrap_fjsonobjectwrapper(value: dict[str, Any]) -> dict[str, Any]:
    """将 FJsonObjectWrapper 的 JsonString 解析为普通 dict。"""
    json_string = value.get("JsonString")
    if not isinstance(json_string, str) or not json_string.strip():
        return value

    try:
        parsed = json.loads(json_string)
    except json.JSONDecodeError:
        return value

    if isinstance(parsed, dict):
        return parsed
    return value


# ============================================================
# 参数校验
# ============================================================

def validate_required_string(value, field_name: str) -> Optional[dict]:
    """
    验证必填字符串参数。
    返回 None 表示通过，返回 dict 表示错误响应（status=validation_error）。
    """
    if not value or not isinstance(value, str) or len(value.strip()) == 0:
        return make_response(
            status="validation_error",
            summary=f"Missing or invalid required field: {field_name}",
            data={},
            errors=[make_error("INVALID_ARGS", f"{field_name} must be a non-empty string")],
        )
    return None


def validate_transform(transform: dict) -> Optional[dict]:
    """
    验证 transform 参数格式。

    UE5 要求 transform 包含：
    - location: [x, y, z] — 对应 AActor::GetActorLocation() 的 FVector
    - rotation: [pitch, yaw, roll] — 对应 AActor::GetActorRotation() 的 FRotator
    - relative_scale3d: [x, y, z] — 对应 AActor::GetActorScale3D() 的 FVector

    返回 None 表示通过，返回 dict 表示错误响应。
    """
    if not isinstance(transform, dict):
        return make_response(
            status="validation_error",
            summary="transform must be a dict",
            data={},
            errors=[make_error("INVALID_ARGS", "transform must be a dict")],
        )

    required_keys = ["location", "rotation", "relative_scale3d"]
    for key in required_keys:
        if key not in transform:
            return make_response(
                status="validation_error",
                summary=f"Transform missing required field: {key}",
                data={},
                errors=[make_error("INVALID_ARGS", f"transform.{key} is required")],
            )
        val = transform[key]
        if not isinstance(val, list) or len(val) != 3:
            return make_response(
                status="validation_error",
                summary=f"transform.{key} must be a 3-element number array",
                data={},
                errors=[make_error("INVALID_ARGS", f"transform.{key} must be [x, y, z]")],
            )
        if not all(isinstance(v, (int, float)) for v in val):
            return make_response(
                status="validation_error",
                summary=f"transform.{key} contains non-numeric values",
                data={},
                errors=[make_error("INVALID_ARGS", f"transform.{key} values must be numbers")],
            )
    return None


# ============================================================
# 包内资源路径
# ============================================================

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
SCHEMAS_DIR = PACKAGE_ROOT / "schemas"
SPEC_TEMPLATES_DIR = PACKAGE_ROOT / "spec_templates"
EXAMPLES_DIR = SCHEMAS_DIR / "examples"


# ============================================================
# Mock 模式
# ============================================================

MOCK_MAP = {
    "get_current_project_state": "get_current_project_state.example.json",
    "list_level_actors": "list_level_actors.example.json",
    "get_actor_state": "get_actor_state.example.json",
    "get_actor_bounds": "get_actor_bounds.example.json",
    "get_asset_metadata": "get_asset_metadata.example.json",
    "get_dirty_assets": "get_dirty_assets.example.json",
    "run_map_check": "run_map_check.example.json",
    "write_operation_feedback": "write_operation_feedback.example.json",
}


def get_mock_response(tool_name: str) -> dict:
    """
    返回对应 example JSON 作为 mock 响应。
    Mock 响应已通过 validate_examples.py Schema 校验。
    仅用于开发和调试，不用于验收。
    """
    filename = MOCK_MAP.get(tool_name)
    if not filename:
        return make_response(
            status="failed",
            summary=f"No mock available for {tool_name}",
            data={},
            errors=[make_error("TOOL_EXECUTION_FAILED", f"Mock not found: {tool_name}")],
        )
    example_path = EXAMPLES_DIR / filename
    if not example_path.exists():
        return make_response(
            status="failed",
            summary=f"Mock file not found: {example_path}",
            data={},
            errors=[make_error("TOOL_EXECUTION_FAILED", f"File not found: {example_path}")],
        )
    return json.loads(example_path.read_text(encoding="utf-8"))
