"""
write_tools.py
==============
AGENT + UE5 可操作層 — L1 语义工具：写类（4 个核心写接口）。

工具层次：L1 语义工具（默认主干，全部使用 FScopedTransaction）
L3 UI 工具见 ui_tools.py（仅当 L1 无法覆盖时使用）

三通道实现：
  通道 A: Python Editor Scripting（unreal 模块进程内调用）
  通道 B: Remote Control API（HTTP + generateTransaction 纳入 Undo）
  通道 C: C++ Plugin（AgentBridge Subsystem，FScopedTransaction 原生 Undo）
  Mock: 返回 write_operation_feedback example JSON

关键原则：
  - 写后必须读回 actual_*（从 UE5 API 重新读取，不是复制输入参数）
  - 通道 B 写操作默认 generateTransaction=True
  - 通道 C 写操作由 C++ FScopedTransaction 自动管理 Undo
  - 所有返回值符合 write_operation_feedback.response.schema.json
"""

from __future__ import annotations

try:
    from .bridge_core import (
        get_channel, BridgeChannel,
        make_response, make_error, safe_execute, get_mock_response,
        validate_required_string, validate_transform, call_cpp_plugin,
    )
    from . import remote_control_client as _rc
    from . import ue_helpers as _ue
except ImportError:
    from bridge_core import (
        get_channel, BridgeChannel,
        make_response, make_error, safe_execute, get_mock_response,
        validate_required_string, validate_transform, call_cpp_plugin,
    )
    import remote_control_client as _rc
    import ue_helpers as _ue


# ============================================================
# 通道分发
# ============================================================

# 写接口在 C++ Plugin 中的函数名映射
_CPP_WRITE_MAP = {
    "spawn_actor": "SpawnActor",
    "set_actor_transform": "SetActorTransform",
    "import_assets": "ImportAssets",
    "create_blueprint_child": "CreateBlueprintChild",
    "set_actor_collision": "SetActorCollision",
    "assign_material": "AssignMaterial",
}


def _dispatch_write(tool_name, func_python, func_rc, *args,
                    cpp_params=None, **kwargs) -> dict:
    """根据当前通道分发写操作。

    通道 C (CPP_PLUGIN) 的写操作由 C++ 端的 FScopedTransaction 管理 Undo，
    Python 端无需额外处理 Transaction。
    """
    channel = get_channel()
    if channel == BridgeChannel.MOCK:
        return get_mock_response("write_operation_feedback")
    elif channel == BridgeChannel.PYTHON_EDITOR:
        return safe_execute(func_python, *args, **kwargs)
    elif channel == BridgeChannel.REMOTE_CONTROL:
        return safe_execute(func_rc, *args, **kwargs)
    elif channel == BridgeChannel.CPP_PLUGIN:
        cpp_func = _CPP_WRITE_MAP.get(tool_name)
        if not cpp_func:
            return make_response(
                status="failed", summary=f"No C++ mapping for: {tool_name}",
                data={}, errors=[make_error("INVALID_ARGS", f"Unknown tool: {tool_name}")])
        return call_cpp_plugin(cpp_func, cpp_params)
    else:
        return make_response(
            status="failed", summary=f"Unknown channel: {channel}",
            data={}, errors=[make_error("INVALID_ARGS", str(channel))])


_EMPTY_WRITE_DATA = {
    "created_objects": [], "modified_objects": [], "deleted_objects": [],
    "dirty_assets": [], "validation": {},
}


# ============================================================
# 1. spawn_actor
# UE5 依赖: UEditorLevelLibrary::SpawnActorFromClass()
# ============================================================

def spawn_actor(level_path: str, actor_class: str, actor_name: str,
                transform: dict, dry_run: bool = False) -> dict:
    err = validate_required_string(level_path, "level_path")
    if err: return err
    err = validate_required_string(actor_class, "actor_class")
    if err: return err
    err = validate_required_string(actor_name, "actor_name")
    if err: return err
    err = validate_transform(transform)
    if err: return err

    return _dispatch_write(
        "spawn_actor",
        lambda: _spawn_actor_python(level_path, actor_class, actor_name, transform, dry_run),
        lambda: _spawn_actor_rc(level_path, actor_class, actor_name, transform, dry_run),
        cpp_params={
            "LevelPath": level_path,
            "ActorClass": actor_class,
            "ActorName": actor_name,
            "Transform": {
                "Location": {"X": transform["location"][0], "Y": transform["location"][1], "Z": transform["location"][2]},
                "Rotation": {"Pitch": transform["rotation"][0], "Yaw": transform["rotation"][1], "Roll": transform["rotation"][2]},
                "RelativeScale3D": {"X": transform["relative_scale3d"][0], "Y": transform["relative_scale3d"][1], "Z": transform["relative_scale3d"][2]},
            },
            "bDryRun": dry_run,
        },
    )


def _spawn_actor_python(level_path, actor_class, actor_name, transform, dry_run) -> dict:
    if dry_run:
        return make_response(status="success", summary=f"Dry run: would spawn {actor_name}",
                             data=dict(_EMPTY_WRITE_DATA))

    import unreal
    ready_err = _ue.check_editor_ready()
    if ready_err: return ready_err

    cls = unreal.load_class(None, actor_class)
    if cls is None:
        return make_response(status="failed", summary=f"Class not found: {actor_class}",
                             data=dict(_EMPTY_WRITE_DATA),
                             errors=[make_error("CLASS_NOT_FOUND", f"Cannot load: {actor_class}")])

    loc = transform["location"]
    rot = transform["rotation"]
    scale = transform["relative_scale3d"]

    location = unreal.Vector(loc[0], loc[1], loc[2])
    rotation = unreal.Rotator(rot[0], rot[1], rot[2])

    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, location, rotation)
    if actor is None:
        return make_response(status="failed", summary="Failed to spawn actor",
                             data=dict(_EMPTY_WRITE_DATA),
                             errors=[make_error("TOOL_EXECUTION_FAILED", "spawn returned None")])

    actor.set_actor_label(actor_name)
    actor.set_actor_scale3d(unreal.Vector(scale[0], scale[1], scale[2]))

    # 写后读回（从 UE5 API 重新读取）
    al = actor.get_actor_location()
    ar = actor.get_actor_rotation()
    asc = actor.get_actor_scale3d()

    return make_response(
        status="success", summary=f"Spawned actor: {actor_name}",
        data={
            "created_objects": [{"actor_name": actor_name, "actor_path": actor.get_path_name()}],
            "modified_objects": [], "deleted_objects": [],
            "actual_transform": {
                "location": [al.x, al.y, al.z],
                "rotation": [ar.pitch, ar.yaw, ar.roll],
                "relative_scale3d": [asc.x, asc.y, asc.z],
            },
            "dirty_assets": [level_path],
            "validation": {},
        },
    )


def _spawn_actor_rc(level_path, actor_class, actor_name, transform, dry_run) -> dict:
    if dry_run:
        return make_response(status="success", summary=f"Dry run: would spawn {actor_name}",
                             data=dict(_EMPTY_WRITE_DATA))

    if not _rc.check_connection():
        return make_response(status="failed", summary="Remote Control not reachable",
                             data=dict(_EMPTY_WRITE_DATA),
                             errors=[make_error("EDITOR_NOT_READY", "RC API not reachable")])

    loc = transform["location"]
    rot = transform["rotation"]
    scale = transform["relative_scale3d"]

    # 生成 Actor（纳入 Undo 事务）
    spawn_resp = _rc.rc_spawn_actor_from_class(
        actor_class=actor_class,
        location={"X": loc[0], "Y": loc[1], "Z": loc[2]},
        rotation={"Pitch": rot[0], "Yaw": rot[1], "Roll": rot[2]},
        generate_transaction=True,
    )
    actor_path = str(spawn_resp.get("ReturnValue", ""))
    if not actor_path:
        return make_response(status="failed", summary="Spawn returned empty path",
                             data=dict(_EMPTY_WRITE_DATA),
                             errors=[make_error("TOOL_EXECUTION_FAILED", "empty ReturnValue")])

    # 设置缩放（纳入 Undo）
    _rc.set_property(actor_path, "RelativeScale3D",
                 {"X": scale[0], "Y": scale[1], "Z": scale[2]},
                 generate_transaction=True)

    # 写后读回
    rl = _rc.get_property(actor_path, "RelativeLocation").get("RelativeLocation", {})
    rr = _rc.get_property(actor_path, "RelativeRotation").get("RelativeRotation", {})
    rs = _rc.get_property(actor_path, "RelativeScale3D").get("RelativeScale3D", {})

    return make_response(
        status="success", summary=f"Spawned actor via RC: {actor_name}",
        data={
            "created_objects": [{"actor_name": actor_name, "actor_path": actor_path}],
            "modified_objects": [], "deleted_objects": [],
            "actual_transform": {
                "location": [rl.get("X", 0), rl.get("Y", 0), rl.get("Z", 0)],
                "rotation": [rr.get("Pitch", 0), rr.get("Yaw", 0), rr.get("Roll", 0)],
                "relative_scale3d": [rs.get("X", 1), rs.get("Y", 1), rs.get("Z", 1)],
            },
            "dirty_assets": [level_path],
            "validation": {},
            "transaction": True,
        },
    )


# ============================================================
# 2. set_actor_transform
# UE5 依赖: AActor::SetActorLocationAndRotation() + SetActorScale3D()
# ============================================================

def set_actor_transform(actor_path: str, transform: dict,
                        dry_run: bool = False) -> dict:
    err = validate_required_string(actor_path, "actor_path")
    if err: return err
    err = validate_transform(transform)
    if err: return err

    return _dispatch_write(
        "set_actor_transform",
        lambda: _set_actor_transform_python(actor_path, transform, dry_run),
        lambda: _set_actor_transform_rc(actor_path, transform, dry_run),
        cpp_params={
            "ActorPath": actor_path,
            "Transform": {
                "Location": {"X": transform["location"][0], "Y": transform["location"][1], "Z": transform["location"][2]},
                "Rotation": {"Pitch": transform["rotation"][0], "Yaw": transform["rotation"][1], "Roll": transform["rotation"][2]},
                "RelativeScale3D": {"X": transform["relative_scale3d"][0], "Y": transform["relative_scale3d"][1], "Z": transform["relative_scale3d"][2]},
            },
            "bDryRun": dry_run,
        },
    )


def _set_actor_transform_python(actor_path, transform, dry_run) -> dict:
    import unreal

    ready_err = _ue.check_editor_ready()
    if ready_err: return ready_err

    actor = _ue.find_actor_by_path(actor_path)
    if actor is None:
        return make_response(status="failed", summary="Actor not found",
                             data=dict(_EMPTY_WRITE_DATA),
                             errors=[make_error("ACTOR_NOT_FOUND", f"No actor at: {actor_path}")])

    old_transform = _ue.read_transform(actor)

    if dry_run:
        data = dict(_EMPTY_WRITE_DATA)
        data["modified_objects"] = [{"actor_path": actor_path}]
        data["old_transform"] = old_transform
        return make_response(status="success", summary="Dry run: would modify transform", data=data)

    loc = transform["location"]
    rot = transform["rotation"]
    scale = transform["relative_scale3d"]

    actor.set_actor_location_and_rotation(
        unreal.Vector(loc[0], loc[1], loc[2]),
        unreal.Rotator(rot[0], rot[1], rot[2]),
        sweep=False, teleport=True)
    actor.set_actor_scale3d(unreal.Vector(scale[0], scale[1], scale[2]))

    actual_transform = _ue.read_transform(actor)
    world = unreal.EditorLevelLibrary.get_editor_world()
    level = world.get_path_name() if world else ""

    return make_response(
        status="success", summary="Transform updated",
        data={
            "created_objects": [],
            "modified_objects": [{"actor_path": actor_path}],
            "deleted_objects": [],
            "old_transform": old_transform,
            "actual_transform": actual_transform,
            "dirty_assets": [level],
            "validation": {},
        },
    )


def _set_actor_transform_rc(actor_path, transform, dry_run) -> dict:
    if not _rc.check_connection():
        return make_response(status="failed", summary="Remote Control not reachable",
                             data=dict(_EMPTY_WRITE_DATA),
                             errors=[make_error("EDITOR_NOT_READY", "RC API not reachable")])

    # 读旧值
    old_loc = _rc.get_property(actor_path, "RelativeLocation").get("RelativeLocation", {})
    old_rot = _rc.get_property(actor_path, "RelativeRotation").get("RelativeRotation", {})
    old_scale = _rc.get_property(actor_path, "RelativeScale3D").get("RelativeScale3D", {})

    old_transform = {
        "location": [old_loc.get("X", 0), old_loc.get("Y", 0), old_loc.get("Z", 0)],
        "rotation": [old_rot.get("Pitch", 0), old_rot.get("Yaw", 0), old_rot.get("Roll", 0)],
        "relative_scale3d": [old_scale.get("X", 1), old_scale.get("Y", 1), old_scale.get("Z", 1)],
    }

    if dry_run:
        data = dict(_EMPTY_WRITE_DATA)
        data["modified_objects"] = [{"actor_path": actor_path}]
        data["old_transform"] = old_transform
        return make_response(status="success", summary="Dry run: would modify transform", data=data)

    loc = transform["location"]
    rot = transform["rotation"]
    scale = transform["relative_scale3d"]

    # 写新值（纳入 Undo）
    _rc.set_property(actor_path, "RelativeLocation",
                 {"X": loc[0], "Y": loc[1], "Z": loc[2]}, generate_transaction=True)
    _rc.set_property(actor_path, "RelativeRotation",
                 {"Pitch": rot[0], "Yaw": rot[1], "Roll": rot[2]}, generate_transaction=True)
    _rc.set_property(actor_path, "RelativeScale3D",
                 {"X": scale[0], "Y": scale[1], "Z": scale[2]}, generate_transaction=True)

    # 读回
    rl = _rc.get_property(actor_path, "RelativeLocation").get("RelativeLocation", {})
    rr = _rc.get_property(actor_path, "RelativeRotation").get("RelativeRotation", {})
    rs = _rc.get_property(actor_path, "RelativeScale3D").get("RelativeScale3D", {})

    return make_response(
        status="success", summary="Transform updated via RC",
        data={
            "created_objects": [],
            "modified_objects": [{"actor_path": actor_path}],
            "deleted_objects": [],
            "old_transform": old_transform,
            "actual_transform": {
                "location": [rl.get("X", 0), rl.get("Y", 0), rl.get("Z", 0)],
                "rotation": [rr.get("Pitch", 0), rr.get("Yaw", 0), rr.get("Roll", 0)],
                "relative_scale3d": [rs.get("X", 1), rs.get("Y", 1), rs.get("Z", 1)],
            },
            "dirty_assets": [],
            "validation": {},
            "transaction": True,
        },
    )


# ============================================================
# 3. import_assets
# UE5 依赖: UAssetToolsHelpers + ImportAssetTasks
# 注意: 当前仅通道 A 支持，通道 B fallback
# ============================================================

def import_assets(source_dir: str, dest_path: str,
                  replace_existing: bool = False, dry_run: bool = False) -> dict:
    err = validate_required_string(source_dir, "source_dir")
    if err: return err
    err = validate_required_string(dest_path, "dest_path")
    if err: return err

    channel = get_channel()
    if channel == BridgeChannel.MOCK:
        return get_mock_response("write_operation_feedback")
    elif channel == BridgeChannel.PYTHON_EDITOR:
        return safe_execute(_import_assets_python, source_dir, dest_path, replace_existing, dry_run)
    elif channel == BridgeChannel.REMOTE_CONTROL:
        return make_response(
            status="failed",
            summary="import_assets not yet supported via Remote Control. Use channel A.",
            data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("TOOL_EXECUTION_FAILED",
                              "ImportAssetTasks requires Python Editor Scripting (channel A)")],
        )
    else:
        return make_response(status="failed", summary=f"Unknown channel", data=dict(_EMPTY_WRITE_DATA))


def _import_assets_python(source_dir, dest_path, replace_existing, dry_run) -> dict:
    import os
    import unreal

    source_files = [os.path.join(source_dir, f) for f in os.listdir(source_dir)
                    if f.endswith(('.fbx', '.obj', '.png', '.tga', '.wav'))]

    if dry_run:
        would_import = [
            {"source_file": f, "dest_asset_path": f"{dest_path}/{os.path.splitext(os.path.basename(f))[0]}"}
            for f in source_files
        ]
        data = dict(_EMPTY_WRITE_DATA)
        data["would_import"] = would_import
        return make_response(status="success",
                             summary=f"Dry run: would import {len(would_import)} assets", data=data)

    tasks = []
    for source_file in source_files:
        task = unreal.AssetImportTask()
        task.filename = source_file
        task.destination_path = dest_path
        task.replace_existing = replace_existing
        task.automated = True
        task.save = False
        tasks.append(task)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    created = []
    dirty = []
    for task in tasks:
        for imported_path in task.imported_object_paths:
            created.append({"asset_path": str(imported_path)})
            dirty.append(str(imported_path))

    return make_response(
        status="success", summary=f"Imported {len(created)} assets",
        data={
            "created_objects": created, "modified_objects": [], "deleted_objects": [],
            "dirty_assets": dirty, "validation": {},
        },
    )


# ============================================================
# 4. create_blueprint_child
# UE5 依赖: UBlueprintFactory + UAssetTools::CreateAsset()
# 注意: 当前仅通道 A 支持，通道 B fallback
# ============================================================

def create_blueprint_child(parent_class: str, package_path: str,
                           dry_run: bool = False) -> dict:
    err = validate_required_string(parent_class, "parent_class")
    if err: return err
    err = validate_required_string(package_path, "package_path")
    if err: return err

    channel = get_channel()
    if channel == BridgeChannel.MOCK:
        return get_mock_response("write_operation_feedback")
    elif channel == BridgeChannel.PYTHON_EDITOR:
        return safe_execute(_create_blueprint_child_python, parent_class, package_path, dry_run)
    elif channel == BridgeChannel.REMOTE_CONTROL:
        return make_response(
            status="failed",
            summary="create_blueprint_child not yet supported via Remote Control. Use channel A.",
            data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("TOOL_EXECUTION_FAILED",
                              "BlueprintFactory requires Python Editor Scripting (channel A)")],
        )
    else:
        return make_response(status="failed", summary="Unknown channel", data=dict(_EMPTY_WRITE_DATA))


def _create_blueprint_child_python(parent_class, package_path, dry_run) -> dict:
    if dry_run:
        return make_response(status="success",
                             summary=f"Dry run: would create BP child at {package_path}",
                             data=dict(_EMPTY_WRITE_DATA))

    import unreal
    parent = unreal.load_class(None, parent_class)
    if parent is None:
        return make_response(
            status="failed", summary=f"Parent class not found: {parent_class}",
            data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("CLASS_NOT_FOUND", f"Cannot load: {parent_class}")])

    parts = package_path.rsplit("/", 1)
    path = parts[0] if len(parts) > 1 else "/Game"
    name = parts[-1]

    factory = unreal.BlueprintFactory()
    factory.parent_class = parent

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    new_asset = asset_tools.create_asset(name, path, unreal.Blueprint, factory)

    if new_asset is None:
        return make_response(
            status="failed", summary="Failed to create Blueprint",
            data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("TOOL_EXECUTION_FAILED", "create_asset returned None")])

    created_path = new_asset.get_path_name()
    return make_response(
        status="success", summary=f"Created Blueprint at {created_path}",
        data={
            "created_objects": [{"asset_path": created_path}],
            "modified_objects": [], "deleted_objects": [],
            "dirty_assets": [created_path], "validation": {},
        },
    )


# ============================================================
# 5. set_actor_collision
# UE5 依赖: UPrimitiveComponent::SetCollisionProfileName / SetCollisionEnabled
# ============================================================

def set_actor_collision(actor_path: str, profile_name: str,
                        collision_enabled: str = "QueryAndPhysics",
                        can_affect_navigation: bool = True,
                        dry_run: bool = False) -> dict:
    err = validate_required_string(actor_path, "actor_path")
    if err:
        return err
    err = validate_required_string(profile_name, "profile_name")
    if err:
        return err

    return _dispatch_write(
        "set_actor_collision",
        lambda: _set_actor_collision_python(actor_path, profile_name, collision_enabled, can_affect_navigation, dry_run),
        lambda: _set_actor_collision_rc(actor_path, profile_name, collision_enabled, can_affect_navigation, dry_run),
        cpp_params={
            "ActorPath": actor_path,
            "CollisionProfileName": profile_name,
            "CollisionEnabledName": collision_enabled,
            "bCanAffectNavigation": can_affect_navigation,
            "bDryRun": dry_run,
        },
    )


def _set_actor_collision_python(actor_path, profile_name, collision_enabled, can_affect_navigation, dry_run) -> dict:
    import unreal
    ready_err = _ue.check_editor_ready()
    if ready_err:
        return ready_err

    actor = _ue.find_actor_by_path(actor_path)
    if actor is None:
        return make_response(
            status="failed", summary="Actor not found", data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("ACTOR_NOT_FOUND", f"No actor at: {actor_path}")])

    component = actor.get_root_component()
    if component is None or not isinstance(component, unreal.PrimitiveComponent):
        return make_response(
            status="failed", summary="PrimitiveComponent not found", data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("TOOL_EXECUTION_FAILED", f"Actor '{actor_path}' root component is not a PrimitiveComponent")])

    old_collision = _ue.read_collision(actor)
    if dry_run:
        data = dict(_EMPTY_WRITE_DATA)
        data["modified_objects"] = [{"actor_path": actor_path}]
        data["old_collision"] = old_collision
        data["preview_collision"] = {
            **old_collision,
            "collision_profile_name": profile_name,
            "collision_enabled": collision_enabled,
            "can_affect_navigation": can_affect_navigation,
        }
        return make_response(status="success", summary="Dry run: would modify collision", data=data)

    component.set_collision_profile_name(profile_name)
    component.set_collision_enabled(getattr(unreal.CollisionEnabled, collision_enabled))
    component.set_can_ever_affect_navigation(can_affect_navigation)

    data = dict(_EMPTY_WRITE_DATA)
    data["modified_objects"] = [{"actor_path": actor_path}]
    data["old_collision"] = old_collision
    data["actual_collision"] = _ue.read_collision(actor)
    return make_response(status="success", summary="Collision updated", data=data)


def _set_actor_collision_rc(actor_path, profile_name, collision_enabled, can_affect_navigation, dry_run) -> dict:
    return make_response(
        status="failed",
        summary="set_actor_collision not supported via direct Remote Control",
        data=dict(_EMPTY_WRITE_DATA),
        errors=[make_error("TOOL_EXECUTION_FAILED", "Use channel C (cpp_plugin) for set_actor_collision")],
    )


# ============================================================
# 6. assign_material
# UE5 依赖: UMeshComponent::SetMaterial
# ============================================================

def assign_material(actor_path: str, material_path: str,
                    slot_index: int = 0, dry_run: bool = False) -> dict:
    err = validate_required_string(actor_path, "actor_path")
    if err:
        return err
    err = validate_required_string(material_path, "material_path")
    if err:
        return err

    return _dispatch_write(
        "assign_material",
        lambda: _assign_material_python(actor_path, material_path, slot_index, dry_run),
        lambda: _assign_material_rc(actor_path, material_path, slot_index, dry_run),
        cpp_params={
            "ActorPath": actor_path,
            "MaterialPath": material_path,
            "SlotIndex": slot_index,
            "bDryRun": dry_run,
        },
    )


def _assign_material_python(actor_path, material_path, slot_index, dry_run) -> dict:
    import unreal
    ready_err = _ue.check_editor_ready()
    if ready_err:
        return ready_err

    actor = _ue.find_actor_by_path(actor_path)
    if actor is None:
        return make_response(
            status="failed", summary="Actor not found", data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("ACTOR_NOT_FOUND", f"No actor at: {actor_path}")])

    mesh_component = None
    components = actor.get_components_by_class(unreal.MeshComponent)
    if components:
        mesh_component = components[0]

    if mesh_component is None:
        return make_response(
            status="failed", summary="MeshComponent not found", data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("TOOL_EXECUTION_FAILED", f"Actor '{actor_path}' has no MeshComponent")])

    material = unreal.EditorAssetLibrary.load_asset(material_path)
    if material is None:
        return make_response(
            status="failed", summary=f"Material not found: {material_path}", data=dict(_EMPTY_WRITE_DATA),
            errors=[make_error("ASSET_NOT_FOUND", f"Cannot load material: {material_path}")])

    old_material = mesh_component.get_material(slot_index)
    if dry_run:
        data = dict(_EMPTY_WRITE_DATA)
        data["modified_objects"] = [{"actor_path": actor_path}]
        data["old_material_path"] = old_material.get_path_name() if old_material else ""
        data["preview_material_path"] = material.get_path_name()
        data["slot_index"] = slot_index
        return make_response(status="success", summary="Dry run: would assign material", data=data)

    mesh_component.set_material(slot_index, material)
    actual_material = mesh_component.get_material(slot_index)
    data = dict(_EMPTY_WRITE_DATA)
    data["modified_objects"] = [{"actor_path": actor_path}]
    data["old_material_path"] = old_material.get_path_name() if old_material else ""
    data["actual_material_path"] = actual_material.get_path_name() if actual_material else ""
    data["actual_material_name"] = actual_material.get_name() if actual_material else ""
    data["slot_index"] = slot_index
    return make_response(status="success", summary="Material assigned", data=data)


def _assign_material_rc(actor_path, material_path, slot_index, dry_run) -> dict:
    return make_response(
        status="failed",
        summary="assign_material not supported via direct Remote Control",
        data=dict(_EMPTY_WRITE_DATA),
        errors=[make_error("TOOL_EXECUTION_FAILED", "Use channel C (cpp_plugin) for assign_material")],
    )
