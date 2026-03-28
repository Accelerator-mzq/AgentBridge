"""
ue_helpers.py
=============
通道 A (Python Editor Scripting) 专用辅助函数。

这些函数直接调用 UE5 官方 `unreal` 模块暴露的 API。
通道 B (Remote Control API) 不需要此模块——它通过 HTTP 调用同一套 UE5 底层 API。

UE5 官方模块依赖：
  - Python Editor Script Plugin（提供 unreal 模块）
  - Editor Scripting Utilities Plugin（提供 EditorLevelLibrary 等高层 API）
"""

from __future__ import annotations

from typing import Optional, List

try:
    import unreal
    HAS_UNREAL = True
except ImportError:
    HAS_UNREAL = False


def find_actor_by_path(actor_path: str) -> Optional["unreal.Actor"]:
    """
    通过完整路径查找 Actor。
    UE5 API: 遍历 UEditorLevelLibrary::GetAllLevelActors()，匹配 get_path_name()
    """
    if not HAS_UNREAL:
        return None
    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    for actor in actors:
        if actor.get_path_name() == actor_path:
            return actor
    return None


def find_actor_by_label(label: str) -> Optional["unreal.Actor"]:
    """
    通过显示名称查找 Actor（备选）。
    UE5 API: AActor::GetActorLabel()
    """
    if not HAS_UNREAL:
        return None
    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    for actor in actors:
        if actor.get_actor_label() == label:
            return actor
    return None


def read_transform(actor: "unreal.Actor") -> dict:
    """
    读取 Actor Transform，返回统一格式。
    UE5 API:
      AActor::GetActorLocation() → FVector (cm)
      AActor::GetActorRotation() → FRotator (pitch/yaw/roll, degrees)
      AActor::GetActorScale3D()  → FVector (倍率)
    """
    loc = actor.get_actor_location()
    rot = actor.get_actor_rotation()
    scale = actor.get_actor_scale3d()
    return {
        "location": [loc.x, loc.y, loc.z],
        "rotation": [rot.pitch, rot.yaw, rot.roll],
        "relative_scale3d": [scale.x, scale.y, scale.z],
    }


def read_collision(actor: "unreal.Actor") -> dict:
    """
    读取 Actor 碰撞状态。
    UE5 API:
      UPrimitiveComponent::GetCollisionProfileName() → FName
      UPrimitiveComponent::GetCollisionEnabled() → ECollisionEnabled
      UPrimitiveComponent::GetGenerateOverlapEvents() → bool
      UPrimitiveComponent::CanEverAffectNavigation() → bool
      UBoxComponent::GetUnscaledBoxExtent() → FVector (仅 BoxComponent)
    """
    collision = {}
    root = actor.root_component
    if root and hasattr(root, "get_collision_profile_name"):
        collision["collision_profile_name"] = str(root.get_collision_profile_name())
        collision["collision_enabled"] = str(root.get_collision_enabled())
        collision["generate_overlap_events"] = root.get_generate_overlap_events()
        collision["can_affect_navigation"] = root.can_ever_affect_navigation()
        # collision_box_extent 仅适用于 BoxComponent
        if hasattr(root, "get_unscaled_box_extent"):
            ext = root.get_unscaled_box_extent()
            collision["collision_box_extent"] = [ext.x, ext.y, ext.z]
    return collision


def read_tags(actor: "unreal.Actor") -> List[str]:
    """
    读取 Actor Tags。
    UE5 API: AActor::Tags (TArray<FName>)
    """
    return [str(tag) for tag in actor.tags]


def check_editor_ready() -> Optional[dict]:
    """
    检查 Editor 是否处于可写状态（通道 A 专用）。
    UE5 API:
      UEditorLevelLibrary::GetEditorWorld() → 确认有 World
      UEditorLevelLibrary::GetGameWorld() → 确认不在 PIE

    返回 None 表示就绪，返回 dict 表示异常（failed 响应）。
    """
    if not HAS_UNREAL:
        from bridge_core import make_response, make_error
        return make_response(
            status="failed", summary="unreal module not available",
            data={}, errors=[make_error("EDITOR_NOT_READY", "Not running inside UE5 Editor")])
    try:
        world = unreal.EditorLevelLibrary.get_editor_world()
        if world is None:
            from bridge_core import make_response, make_error
            return make_response(
                status="failed", summary="No editor world available",
                data={}, errors=[make_error("EDITOR_NOT_READY", "Editor world is None")])
        game_world = unreal.EditorLevelLibrary.get_game_world()
        if game_world is not None:
            from bridge_core import make_response, make_error
            return make_response(
                status="failed", summary="Editor is in PIE mode",
                data={}, errors=[make_error("EDITOR_NOT_READY", "Cannot write during PIE")])
    except Exception as e:
        from bridge_core import make_response, make_error
        return make_response(
            status="failed", summary=f"Editor check failed: {e}",
            data={}, errors=[make_error("EDITOR_NOT_READY", str(e))])
    return None
