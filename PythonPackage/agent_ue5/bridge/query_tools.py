"""
query_tools.py
==============
AGENT + UE5 可操作層 — L1 语义工具：查询类（7 个核心反馈接口）。

工具层次：L1 语义工具（默认主干，优先级最高）
L3 UI 工具见 ui_tools.py（仅当 L1 无法覆盖时使用）

三通道实现：
  通道 A: Python Editor Scripting（unreal 模块进程内调用）
  通道 B: Remote Control API（HTTP 远程调用原生 UE5 API）
  通道 C: C++ Plugin（通过 RC API 调用 AgentBridge Subsystem，最完整）
  Mock: 返回 agent_ue5/schemas/examples/ 中的预定义 JSON

每个接口的执行能力来自 UE5 官方 API。
通道 C（C++ Plugin）在 C++ 端完成参数校验 + 统一响应 + Transaction，
Python 端仅作为轻量客户端。
"""

from __future__ import annotations

try:
    from .bridge_core import (
        get_channel, BridgeChannel,
        make_response, make_error, safe_execute, get_mock_response,
        validate_required_string, call_cpp_plugin,
    )
    from . import remote_control_client as _rc
    from . import ue_helpers as _ue
except ImportError:
    from bridge_core import (
        get_channel, BridgeChannel,
        make_response, make_error, safe_execute, get_mock_response,
        validate_required_string, call_cpp_plugin,
    )
    import remote_control_client as _rc
    import ue_helpers as _ue


# ============================================================
# 通道分发辅助
# ============================================================

# 查询接口在 C++ Plugin 中的函数名映射
_CPP_QUERY_MAP = {
    "get_current_project_state": "GetCurrentProjectState",
    "list_level_actors": "ListLevelActors",
    "get_actor_state": "GetActorState",
    "get_actor_bounds": "GetActorBounds",
    "get_asset_metadata": "GetAssetMetadata",
    "get_dirty_assets": "GetDirtyAssets",
    "run_map_check": "RunMapCheck",
    "get_component_state": "GetComponentState",
    "get_material_assignment": "GetMaterialAssignment",
}


def _dispatch(tool_name, func_python, func_rc, *args,
              cpp_params=None, **kwargs) -> dict:
    """根据当前通道分发到对应实现。

    通道 A (PYTHON_EDITOR): 调用 func_python（进程内 unreal 模块）
    通道 B (REMOTE_CONTROL): 调用 func_rc（HTTP 调用 UE5 原生 API）
    通道 C (CPP_PLUGIN): 通过 RC API 调用 C++ AgentBridge Subsystem
    MOCK: 返回 example JSON
    """
    channel = get_channel()
    if channel == BridgeChannel.MOCK:
        return get_mock_response(tool_name)
    elif channel == BridgeChannel.PYTHON_EDITOR:
        return safe_execute(func_python, *args, **kwargs)
    elif channel == BridgeChannel.REMOTE_CONTROL:
        return safe_execute(func_rc, *args, **kwargs)
    elif channel == BridgeChannel.CPP_PLUGIN:
        cpp_func = _CPP_QUERY_MAP.get(tool_name)
        if not cpp_func:
            return make_response(
                status="failed", summary=f"No C++ mapping for: {tool_name}",
                data={}, errors=[make_error("INVALID_ARGS", f"Unknown tool: {tool_name}")]
            )
        return call_cpp_plugin(cpp_func, cpp_params)
    else:
        return make_response(
            status="failed", summary=f"Unknown channel: {channel}",
            data={}, errors=[make_error("INVALID_ARGS", f"Unknown channel: {channel}")]
        )


# ============================================================
# 1. get_current_project_state
# UE5 依赖: FPaths + UKismetSystemLibrary + UEditorLevelLibrary
# ============================================================

def get_current_project_state() -> dict:
    return _dispatch("get_current_project_state",
                     _get_current_project_state_python,
                     _get_current_project_state_rc)


def _get_current_project_state_python() -> dict:
    import unreal
    project_path = unreal.Paths.get_project_file_path()
    project_name = unreal.Paths.get_base_filename(project_path)
    world = unreal.EditorLevelLibrary.get_editor_world()
    current_level = world.get_path_name() if world else ""
    engine_version = unreal.SystemLibrary.get_engine_version()
    editor_mode = "editing"
    if unreal.EditorLevelLibrary.get_game_world() is not None:
        editor_mode = "pie"
    return make_response(
        status="success",
        summary="Current project state fetched successfully",
        data={
            "project_name": project_name,
            "uproject_path": str(project_path),
            "engine_version": engine_version,
            "current_level": current_level,
            "editor_mode": editor_mode,
        },
    )


def _get_current_project_state_rc() -> dict:
    version_resp = _rc.call_function(_rc.SYSTEM_LIB, "GetEngineVersion")
    engine_version = version_resp.get("ReturnValue", "")
    world_resp = _rc.call_function(_rc.EDITOR_LEVEL_LIB, "GetEditorWorld")
    current_level = str(world_resp.get("ReturnValue", ""))
    return make_response(
        status="success",
        summary="Current project state fetched via Remote Control",
        data={
            "project_name": "",
            "uproject_path": "",
            "engine_version": engine_version,
            "current_level": current_level,
            "editor_mode": "editing",
        },
    )


# ============================================================
# 2. list_level_actors
# UE5 依赖: UEditorLevelLibrary::GetAllLevelActors()
# ============================================================

def list_level_actors(level_path: str = None, class_filter: str = None) -> dict:
    return _dispatch("list_level_actors",
                     lambda: _list_level_actors_python(level_path, class_filter),
                     lambda: _list_level_actors_rc(level_path, class_filter),
                     cpp_params={"ClassFilter": class_filter or ""})


def _list_level_actors_python(level_path=None, class_filter=None) -> dict:
    import unreal
    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    actor_list = []
    for actor in actors:
        class_path = actor.get_class().get_path_name()
        if class_filter and class_filter not in class_path:
            continue
        actor_list.append({
            "actor_name": actor.get_name(),
            "actor_path": actor.get_path_name(),
            "class": class_path,
        })
    data = {"actors": actor_list}
    if level_path:
        data["level_path"] = level_path
    return make_response(status="success", summary=f"Listed {len(actor_list)} actors", data=data)


def _list_level_actors_rc(level_path=None, class_filter=None) -> dict:
    resp = _rc.rc_get_all_level_actors()
    raw_actors = resp.get("ReturnValue", [])
    actor_list = []
    for actor_path in raw_actors:
        path_str = str(actor_path)
        parts = path_str.rsplit(".", 1)
        actor_name = parts[-1] if len(parts) > 1 else path_str
        actor_list.append({
            "actor_name": actor_name,
            "actor_path": path_str,
            "class": "",
        })
    data = {"actors": actor_list}
    if level_path:
        data["level_path"] = level_path
    return make_response(status="success", summary=f"Listed {len(actor_list)} actors via RC", data=data)


# ============================================================
# 3. get_actor_state
# UE5 依赖: AActor::GetActorLocation/Rotation/Scale3D() + collision + tags
# ============================================================

def get_actor_state(actor_path: str) -> dict:
    err = validate_required_string(actor_path, "actor_path")
    if err:
        return err
    return _dispatch("get_actor_state",
                     lambda: _get_actor_state_python(actor_path),
                     lambda: _get_actor_state_rc(actor_path),
                     cpp_params={"ActorPath": actor_path})


def _get_actor_state_python(actor_path: str) -> dict:
    import unreal
    actor = _ue.find_actor_by_path(actor_path)
    if actor is None:
        return make_response(
            status="failed", summary="Actor not found", data={},
            errors=[make_error("ACTOR_NOT_FOUND", f"No actor at path: {actor_path}")])
    world = unreal.EditorLevelLibrary.get_editor_world()
    return make_response(
        status="success", summary="Actor state fetched successfully",
        data={
            "actor_name": actor.get_name(),
            "actor_path": actor.get_path_name(),
            "class": actor.get_class().get_path_name(),
            "target_level": world.get_path_name() if world else "",
            "transform": _ue.read_transform(actor),
            "collision": _ue.read_collision(actor),
            "tags": _ue.read_tags(actor),
        },
    )


def _get_actor_state_rc(actor_path: str) -> dict:
    try:
        # UE5.5.4 下 ActorPath 走 /remote/object/property 读取 RelativeLocation
        # 会出现“property could not be resolved”的 400。
        # 这里改为直接调用 Actor 自身的原生函数，避免 B 通道在三通道一致性校验里误报失败。
        loc = _rc.call_function(actor_path, "K2_GetActorLocation")
        rot = _rc.call_function(actor_path, "K2_GetActorRotation")
        scale = _rc.call_function(actor_path, "GetActorScale3D")

        location = loc.get("ReturnValue", {})
        rotation = rot.get("ReturnValue", {})
        scale3d = scale.get("ReturnValue", {})

        return make_response(
            status="success", summary="Actor state fetched via Remote Control",
            data={
                "actor_name": actor_path.rsplit(".", 1)[-1] if "." in actor_path else actor_path,
                "actor_path": actor_path,
                "class": "",
                "target_level": "",
                "transform": {
                    "location": [location.get("X", 0), location.get("Y", 0), location.get("Z", 0)],
                    "rotation": [rotation.get("Pitch", 0), rotation.get("Yaw", 0), rotation.get("Roll", 0)],
                    "relative_scale3d": [scale3d.get("X", 1), scale3d.get("Y", 1), scale3d.get("Z", 1)],
                },
                "collision": {},
                "tags": [],
            },
        )
    except _rc.RemoteControlError as e:
        if e.status_code == 404 or "not found" in str(e).lower():
            return make_response(
                status="failed", summary="Actor not found", data={},
                errors=[make_error("ACTOR_NOT_FOUND", f"No actor at path: {actor_path}")])
        raise


# ============================================================
# 4. get_actor_bounds
# UE5 依赖: AActor::GetActorBounds()
# ============================================================

def get_actor_bounds(actor_path: str) -> dict:
    err = validate_required_string(actor_path, "actor_path")
    if err:
        return err
    return _dispatch("get_actor_bounds",
                     lambda: _get_actor_bounds_python(actor_path),
                     lambda: _get_actor_bounds_rc(actor_path),
                     cpp_params={"ActorPath": actor_path})


def _get_actor_bounds_python(actor_path: str) -> dict:
    actor = _ue.find_actor_by_path(actor_path)
    if actor is None:
        return make_response(
            status="failed", summary="Actor not found", data={},
            errors=[make_error("ACTOR_NOT_FOUND", f"No actor at path: {actor_path}")])
    _, origin, extent = actor.get_actor_bounds(only_colliding_components=False)
    return make_response(
        status="success", summary="Actor bounds fetched successfully",
        data={
            "actor_path": actor.get_path_name(),
            "world_bounds_origin": [origin.x, origin.y, origin.z],
            "world_bounds_extent": [extent.x, extent.y, extent.z],
        },
    )


def _get_actor_bounds_rc(actor_path: str) -> dict:
    resp = _rc.call_function(actor_path, "GetActorBounds", {"bOnlyCollidingComponents": False})
    origin = resp.get("Origin", {})
    extent = resp.get("BoxExtent", {})
    return make_response(
        status="success", summary="Actor bounds fetched via Remote Control",
        data={
            "actor_path": actor_path,
            "world_bounds_origin": [origin.get("X", 0), origin.get("Y", 0), origin.get("Z", 0)],
            "world_bounds_extent": [extent.get("X", 0), extent.get("Y", 0), extent.get("Z", 0)],
        },
    )


# ============================================================
# 5. get_asset_metadata
# UE5 依赖: UEditorAssetLibrary::DoesAssetExist() + FindAssetData()
# ============================================================

def get_asset_metadata(asset_path: str) -> dict:
    err = validate_required_string(asset_path, "asset_path")
    if err:
        return err
    return _dispatch("get_asset_metadata",
                     lambda: _get_asset_metadata_python(asset_path),
                     lambda: _get_asset_metadata_rc(asset_path),
                     cpp_params={"AssetPath": asset_path})


def _get_asset_metadata_python(asset_path: str) -> dict:
    import unreal
    exists = unreal.EditorAssetLibrary.does_asset_exist(asset_path)
    data = {"asset_path": asset_path, "class": "", "exists": exists}
    if exists:
        asset_data = unreal.EditorAssetLibrary.find_asset_data(asset_path)
        data["class"] = str(asset_data.asset_class_path) if asset_data else ""
        data["asset_name"] = str(asset_data.asset_name) if asset_data else ""
        asset = unreal.EditorAssetLibrary.load_asset(asset_path)
        if asset and isinstance(asset, unreal.StaticMesh):
            bbox = asset.get_bounding_box()
            center = (bbox.min + bbox.max) / 2.0
            ext = (bbox.max - bbox.min) / 2.0
            data["mesh_asset_bounds"] = {
                "world_bounds_origin": [center.x, center.y, center.z],
                "world_bounds_extent": [ext.x, ext.y, ext.z],
            }
    return make_response(
        status="success",
        summary="Asset metadata fetched" if exists else "Asset not found",
        data=data,
    )


def _get_asset_metadata_rc(asset_path: str) -> dict:
    resp = _rc.call_function(_rc.EDITOR_ASSET_LIB, "DoesAssetExist", {"AssetPath": asset_path})
    exists = resp.get("ReturnValue", False)
    data = {"asset_path": asset_path, "class": "", "exists": exists}
    return make_response(
        status="success",
        summary="Asset metadata fetched via RC" if exists else "Asset not found",
        data=data,
    )


# ============================================================
# 6. get_dirty_assets
# UE5 依赖: UEditorLoadingAndSavingUtils / Package 脏状态管理
# ============================================================

def get_dirty_assets() -> dict:
    return _dispatch("get_dirty_assets",
                     _get_dirty_assets_python,
                     _get_dirty_assets_rc)


def _get_dirty_assets_python() -> dict:
    import unreal
    dirty = []
    try:
        dirty_packages = unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
        dirty = [str(pkg.get_path_name()) for pkg in dirty_packages]
    except AttributeError:
        pass
    return make_response(
        status="success", summary=f"Found {len(dirty)} dirty assets",
        data={"dirty_assets": dirty},
    )


def _get_dirty_assets_rc() -> dict:
    resp = _rc.call_function(
        "/Script/EditorScriptingUtilities.Default__EditorLoadingAndSavingUtils",
        "GetDirtyContentPackages"
    )
    raw = resp.get("ReturnValue", [])
    dirty = [str(p) for p in raw]
    return make_response(
        status="success", summary=f"Found {len(dirty)} dirty assets via RC",
        data={"dirty_assets": dirty},
    )


# ============================================================
# 6.1 get_component_state
# UE5 依赖: Actor->GetComponents() + USceneComponent::GetRelativeTransform()
# ============================================================

def get_component_state(actor_path: str, component_name: str) -> dict:
    err = validate_required_string(actor_path, "actor_path")
    if err:
        return err
    err = validate_required_string(component_name, "component_name")
    if err:
        return err

    if get_channel() == BridgeChannel.MOCK:
        return make_response(
            status="success",
            summary="Component state retrieved",
            data={
                "component_name": component_name,
                "component_class": "/Script/Engine.StaticMeshComponent",
                "relative_location": [0.0, 0.0, 0.0],
                "relative_rotation": [0.0, 0.0, 0.0],
                "relative_scale": [1.0, 1.0, 1.0],
            },
        )

    return _dispatch(
        "get_component_state",
        lambda: _get_component_state_python(actor_path, component_name),
        lambda: _get_component_state_rc(actor_path, component_name),
        cpp_params={"ActorPath": actor_path, "ComponentName": component_name},
    )


def _get_component_state_python(actor_path: str, component_name: str) -> dict:
    import unreal
    actor = _ue.find_actor_by_path(actor_path)
    if actor is None:
        return make_response(
            status="failed", summary="Actor not found", data={},
            errors=[make_error("ACTOR_NOT_FOUND", f"No actor at path: {actor_path}")])

    for component in actor.get_components_by_class(unreal.ActorComponent):
        if component.get_name() != component_name:
            continue

        if not hasattr(component, "get_relative_location"):
            return make_response(
                status="failed", summary="Component is not a scene component", data={},
                errors=[make_error("TOOL_EXECUTION_FAILED", f"Component '{component_name}' has no relative transform")])

        loc = component.get_relative_location()
        rot = component.get_relative_rotation()
        scale = component.get_relative_scale3d()
        return make_response(
            status="success",
            summary="Component state retrieved",
            data={
                "component_name": component.get_name(),
                "component_class": component.get_class().get_path_name(),
                "relative_location": [loc.x, loc.y, loc.z],
                "relative_rotation": [rot.pitch, rot.yaw, rot.roll],
                "relative_scale": [scale.x, scale.y, scale.z],
            },
        )

    return make_response(
        status="failed", summary="Component not found", data={},
        errors=[make_error("TOOL_EXECUTION_FAILED", f"No component named '{component_name}' on actor '{actor_path}'")])


def _get_component_state_rc(actor_path: str, component_name: str) -> dict:
    return make_response(
        status="failed",
        summary="get_component_state not supported via direct Remote Control",
        data={},
        errors=[make_error("TOOL_EXECUTION_FAILED", "Use channel C (cpp_plugin) for get_component_state")],
    )


# ============================================================
# 6.2 get_material_assignment
# UE5 依赖: UMeshComponent::GetNumMaterials / GetMaterial()
# ============================================================

def get_material_assignment(actor_path: str) -> dict:
    err = validate_required_string(actor_path, "actor_path")
    if err:
        return err

    if get_channel() == BridgeChannel.MOCK:
        return make_response(
            status="success",
            summary="Material assignment retrieved",
            data={
                "actor_path": actor_path,
                "materials": [
                    {
                        "slot_index": 0,
                        "material_path": "/Engine/BasicShapes/BasicShapeMaterial",
                        "material_name": "BasicShapeMaterial",
                    }
                ],
            },
        )

    return _dispatch(
        "get_material_assignment",
        lambda: _get_material_assignment_python(actor_path),
        lambda: _get_material_assignment_rc(actor_path),
        cpp_params={"ActorPath": actor_path},
    )


def _get_material_assignment_python(actor_path: str) -> dict:
    import unreal
    actor = _ue.find_actor_by_path(actor_path)
    if actor is None:
        return make_response(
            status="failed", summary="Actor not found", data={},
            errors=[make_error("ACTOR_NOT_FOUND", f"No actor at path: {actor_path}")])

    mesh_component = None
    components = actor.get_components_by_class(unreal.MeshComponent)
    if components:
        mesh_component = components[0]

    if mesh_component is None:
        return make_response(
            status="failed", summary="MeshComponent not found", data={},
            errors=[make_error("TOOL_EXECUTION_FAILED", f"Actor '{actor_path}' has no MeshComponent")])

    materials = []
    num_materials = mesh_component.get_num_materials()
    for slot_index in range(num_materials):
        material = mesh_component.get_material(slot_index)
        materials.append({
            "slot_index": slot_index,
            "material_path": material.get_path_name() if material else "",
            "material_name": material.get_name() if material else "",
        })

    return make_response(
        status="success",
        summary="Material assignment retrieved",
        data={"actor_path": actor_path, "materials": materials},
    )


def _get_material_assignment_rc(actor_path: str) -> dict:
    return make_response(
        status="failed",
        summary="get_material_assignment not supported via direct Remote Control",
        data={},
        errors=[make_error("TOOL_EXECUTION_FAILED", "Use channel C (cpp_plugin) for get_material_assignment")],
    )


# ============================================================
# 7. run_map_check
# UE5 依赖: Editor 内置 MAP CHECK Console Command
# ============================================================

def run_map_check(level_path: str = None) -> dict:
    return _dispatch("run_map_check",
                     lambda: _run_map_check_python(level_path),
                     lambda: _run_map_check_rc(level_path))


def _run_map_check_python(level_path=None) -> dict:
    import unreal
    unreal.SystemLibrary.execute_console_command(
        unreal.EditorLevelLibrary.get_editor_world(), "MAP CHECK")
    # MAP CHECK 结果输出到日志，需要解析
    return make_response(
        status="success", summary="Map check executed",
        data={
            "level_path": level_path or "",
            "map_errors": [],
            "map_warnings": [],
        },
    )


def _run_map_check_rc(level_path=None) -> dict:
    _rc.call_function(_rc.EDITOR_LEVEL_LIB, "EditorExec", {"Command": "MAP CHECK"})
    return make_response(
        status="success", summary="Map check executed via RC",
        data={
            "level_path": level_path or "",
            "map_errors": [],
            "map_warnings": [],
        },
    )
