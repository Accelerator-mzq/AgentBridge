"""
remote_control_client.py
========================
UE5 Remote Control API HTTP 客户端封装。

UE5 官方 Remote Control API 提供 HTTP REST 端点，允许外部程序
远程调用 Editor 中的 BlueprintCallable 函数和读写公开属性。

本客户端封装这些 HTTP 调用，为 Bridge 封装层提供通道 B 的底层能力。

前提：
  - UE5.5.4 Editor 已启动
  - Remote Control API Plugin 已启用
  - Web Server 已启动（默认端口 30010）
"""

from __future__ import annotations

import json
import urllib.request
import urllib.error
from typing import Any, Dict, List, Optional


# ============================================================
# 配置
# ============================================================

DEFAULT_HOST = "http://localhost"
DEFAULT_PORT = 30010


class RemoteControlConfig:
    """Remote Control API 连接配置。"""
    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT):
        self.host = host
        self.port = port
        self.base_url = f"{host}:{port}"


_config = RemoteControlConfig()


def configure(host: str = DEFAULT_HOST, port: int = DEFAULT_PORT):
    """配置 Remote Control API 连接参数。"""
    global _config
    _config = RemoteControlConfig(host, port)


def get_base_url() -> str:
    return _config.base_url


# ============================================================
# 异常类
# ============================================================

class RemoteControlError(Exception):
    """Remote Control API 调用错误。"""
    def __init__(self, message: str, status_code: int = 0, response_body: str = ""):
        super().__init__(message)
        self.status_code = status_code
        self.response_body = response_body


# ============================================================
# 底层 HTTP 调用
# ============================================================

def _http_request(endpoint: str, body: dict = None, method: str = "PUT",
                  timeout: float = 30.0) -> dict:
    url = f"{_config.base_url}{endpoint}"
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(
        url, data=data,
        headers={"Content-Type": "application/json"} if data else {},
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            response_body = resp.read().decode("utf-8")
            return json.loads(response_body) if response_body else {}
    except urllib.error.HTTPError as e:
        error_body = ""
        try:
            error_body = e.read().decode("utf-8") if e.fp else ""
        except Exception:
            pass
        raise RemoteControlError(f"HTTP {e.code}: {e.reason}", e.code, error_body)
    except urllib.error.URLError as e:
        raise RemoteControlError(
            f"Connection failed: {e.reason}. Is UE5 Editor running at {_config.base_url}?")
    except TimeoutError:
        raise RemoteControlError(f"Request timed out after {timeout}s to {url}")


# ============================================================
# 核心 API
# ============================================================

def call_function(object_path: str, function_name: str,
                  parameters: Optional[Dict[str, Any]] = None,
                  generate_transaction: bool = False) -> dict:
    """调用 UObject 的 BlueprintCallable 函数。端点：PUT /remote/object/call"""
    body: Dict[str, Any] = {
        "objectPath": object_path,
        "functionName": function_name,
    }
    if generate_transaction:
        body["generateTransaction"] = True
    if parameters:
        body["parameters"] = parameters
    return _http_request("/remote/object/call", body)


def get_property(object_path: str, property_name: str) -> dict:
    """读取 UObject 公开属性。端点：PUT /remote/object/property (READ)"""
    return _http_request("/remote/object/property", {
        "objectPath": object_path,
        "propertyName": property_name,
        "access": "READ_ACCESS",
    })


def set_property(object_path: str, property_name: str, property_value: Any,
                 generate_transaction: bool = False) -> dict:
    """写入 UObject 公开属性。端点：PUT /remote/object/property (WRITE)"""
    body: Dict[str, Any] = {
        "objectPath": object_path,
        "propertyName": property_name,
        "propertyValue": property_value,
        "access": "WRITE_ACCESS",
    }
    if generate_transaction:
        body["generateTransaction"] = True
    return _http_request("/remote/object/property", body)


def batch(requests: List[dict]) -> List[dict]:
    """批量执行。端点：PUT /remote/batch"""
    result = _http_request("/remote/batch", {"Requests": requests})
    return result.get("Responses", [])


def search_actors(query: str = "", class_name: str = "") -> dict:
    """搜索 Actor。端点：PUT /remote/search/actors"""
    body: Dict[str, str] = {}
    if query: body["Query"] = query
    if class_name: body["Class"] = class_name
    return _http_request("/remote/search/actors", body)


def search_assets(query: str = "", class_name: str = "", path_filter: str = "") -> dict:
    """搜索资产。端点：PUT /remote/search/assets"""
    body: Dict[str, str] = {}
    if query: body["Query"] = query
    if class_name: body["Class"] = class_name
    if path_filter: body["Filter"] = path_filter
    return _http_request("/remote/search/assets", body)


# ============================================================
# 便捷常量
# ============================================================

EDITOR_LEVEL_LIB = "/Script/EditorScriptingUtilities.Default__EditorLevelLibrary"
EDITOR_ASSET_LIB = "/Script/EditorScriptingUtilities.Default__EditorAssetLibrary"
SYSTEM_LIB = "/Script/Engine.Default__KismetSystemLibrary"


# ============================================================
# 便捷方法
# ============================================================

def rc_get_all_level_actors() -> dict:
    return call_function(EDITOR_LEVEL_LIB, "GetAllLevelActors")

def rc_spawn_actor_from_class(actor_class: str, location: dict,
                               rotation: Optional[dict] = None,
                               generate_transaction: bool = True) -> dict:
    params: Dict[str, Any] = {"ActorClass": actor_class, "Location": location}
    if rotation: params["Rotation"] = rotation
    return call_function(EDITOR_LEVEL_LIB, "SpawnActorFromClass",
                         parameters=params, generate_transaction=generate_transaction)


# ============================================================
# 健康检查
# ============================================================

def check_connection() -> bool:
    try:
        _http_request("/remote", method="GET")
        return True
    except Exception:
        return False

def get_available_routes() -> dict:
    return _http_request("/remote", method="GET")
