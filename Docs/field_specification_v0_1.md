# UE5 AGENT 设计文档字段规范 v0.1

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 规范目标

本规范约束 **用户需求 → 设计文档 → Agent 执行** 的字段表达方式。

核心原则：用户可以模糊表达，设计层可以保留语义抽象，但进入 UE5 执行层之前，所有关键对象都必须落成明确、单义、可校验的字段。

**本规范中执行层字段名直接映射自 UE5 官方 API 的属性名。** 这不是我们自创的命名体系——它与 UE5 C++ API 保持一致，确保 Bridge 封装层可以直接映射，无需额外的字段名翻译。

本规范将字段分为三层：

1. **用户语义层** — 面向用户或策划，允许受控的自然表达
2. **设计规范层** — 面向 Spec / Planner，负责消歧义
3. **执行技术层** — 面向 Bridge / Tool Router，只允许明确、单义、可读回字段（字段名 = UE5 API 属性名）

---

## 2. 总体原则

### 2.1 单义原则

一个字段只能表达一种含义。禁止使用 `size`（可能指缩放、占地或碰撞体积）、`position`（与 location 混淆）、`center`（与 world_bounds_origin 混淆）。

### 2.2 分层原则

模糊表达可以存在于用户层，但不能进入执行层。

| 层 | 示例 |
|---|---|
| 用户层 | "大一点" |
| 设计层 | `size_profile: large` |
| 执行层 | `relative_scale3d: [1.25, 1.25, 1.25]` |

### 2.3 可验证原则

所有进入执行层的关键字段，执行后都必须能读回验证。

### 2.4 不直接消费原始自然语言

执行 Agent 不直接根据用户原话操作 UE5，只读取结构化 Spec。

### 2.5 缺失即阻断

当关键字段无法唯一确定时，必须停止执行、触发补全/追问、或使用已声明默认规则。不能"靠猜"执行高影响操作。

---

## 3. 坐标系与单位约定

本规范中的字段名直接映射自 UE5.5.4 官方 C++ API 的属性名和返回值类型。v0.3 中这些映射在 C++ Plugin 的 `BridgeTypes.h` 中以 USTRUCT 形式实现：

| 字段名 | UE5 C++ API | 返回类型 | 单位 | C++ Plugin 结构体 |
|---|---|---|---|---|
| `location` | `AActor::GetActorLocation()` | `FVector` | cm | `FBridgeTransform::Location` |
| `rotation` | `AActor::GetActorRotation()` | `FRotator` (pitch/yaw/roll) | degrees | `FBridgeTransform::Rotation` |
| `relative_scale3d` | `AActor::GetActorScale3D()` | `FVector` | 倍率 | `FBridgeTransform::RelativeScale3D` |
| `world_bounds_origin` | `AActor::GetActorBounds()` → origin | `FVector` | cm | JSON 数组 |
| `world_bounds_extent` | `AActor::GetActorBounds()` → extent | `FVector` | cm | JSON 数组 |
| `collision_box_extent` | `UBoxComponent::GetUnscaledBoxExtent()` | `FVector` | cm (half-extent) | JSON 数组 |
| `collision_profile_name` | `UPrimitiveComponent::GetCollisionProfileName()` | `FName` | — | JSON string |
| `collision_enabled` | `UPrimitiveComponent::GetCollisionEnabled()` | `ECollisionEnabled` | — | JSON string |
| `can_affect_navigation` | `UPrimitiveComponent::CanEverAffectNavigation()` | `bool` | — | JSON bool |

C++ Plugin 中的容差验证使用 `FBridgeTransform::NearlyEquals(Other, LocationTol, RotationTol, ScaleTol)`，替代 v0.2 Python `verifier.py` 的自建比对逻辑。

基于 UE5.5.4 的真实属性：

| 项目 | 约定 |
|---|---|
| 世界单位 | **厘米（cm）** |
| Location | 世界坐标，`[x, y, z]`，单位 cm |
| Rotation | `[pitch, yaw, roll]`，单位**度（degrees）** |
| Scale | `relative_scale3d: [x, y, z]`，倍率（1.0 = 原始大小） |
| Bounds | `world_bounds_origin` / `world_bounds_extent`，单位 cm |
| Collision extent | `collision_box_extent`，单位 cm，表示半径（half-extent） |

默认使用**世界坐标系**。如需相对坐标（组件级），应显式使用 `relative_location` / `relative_rotation` / `relative_scale3d` 并在字段名中标注。

---

## 4. 字段层级定义

### 4.1 用户语义层字段

允许偏自然的表达，但建议控制在预设词表内。

推荐词表：

**placement**：`center_of_room` / `near_wall_left` / `near_wall_right` / `near_back_wall` / `near_entry` / `near_exit` / `along_main_path` / `corner_front_left` / `corner_front_right` / `corner_back_left` / `corner_back_right`

**size_profile**：`small` / `medium` / `large` / `hero_large`

**density**：`sparse` / `medium` / `dense`

**facing**：`north` / `south` / `east` / `west` / `toward_entry` / `toward_exit` / `toward_main_path` / `toward_center`

以下字段仅可停留在用户层，**不可原样传入执行层**：

`big` / `small`（作为自由文本时）/ `near` / `far` / `middle` / `proper` / `looks good` / `a bit left` / `not too big` / `large enough`

**重要说明**：`small` / `large` 等词作为**设计层预设枚举值**（如 `size_profile: small`）是合法的，前提是它们在进入执行层前已通过默认规则映射为具体数值（如 `relative_scale3d: [0.8, 0.8, 0.8]`）。禁止的是将这些词作为**执行层字段名或自由文本值**使用。

---

### 4.2 设计规范层字段

这一层是 Planner / Spec Translator 的输出，要求已基本消歧义。

推荐结构：

```yaml
scene_spec:
  scene_id: abandoned_warehouse_01
  scene_type: indoor_combat
  anchors:
    room_center: [0, 0, 0]
    main_entry: [-1200, 0, 0]
    main_exit: [1200, 0, 0]

  actors:
    - id: truck_01
      archetype: hero_vehicle
      placement_rule: center_of_room
      facing_rule: east
      size_profile: large
      traversal_impact: high
```

设计层 → 执行层转换规则框架：

| 设计层字段 | 转换方式 | 执行层字段 |
|---|---|---|
| `size_profile` | 通过 `size_profile_to_scale` 默认规则映射 | `relative_scale3d` |
| `placement_rule` + `spawn_anchor_id` + `placement_offset` | 锚点坐标 + 偏移量计算 | `location` |
| `facing_rule: fixed_yaw` + `yaw_degrees` | 直接映射 | `rotation: [0, yaw, 0]` |
| `facing_rule: toward_center` | 根据当前 location 与目标点计算 yaw | `rotation: [0, computed_yaw, 0]` |
| `density` | 通过 `density_to_spacing` 映射为间距值 | 用于计算多个对象的 `location` |

当前 Spec Translator 由 Agent 内部完成，不独立部署。

---

### 4.3 执行技术层字段

只允许机器稳定执行和校验的字段。

---

## 5. 执行技术层字段规范

### 5.1 通用对象标识字段

每个对象都必须有稳定标识。

| 字段 | 说明 | 必填 |
|---|---|---|
| `id` | 文档内唯一 ID | 是 |
| `class` | UE 类路径 | 是 |
| `object_type` | 对象类别（actor / asset） | 是 |
| `target_level` | 目标关卡路径 | 是 |
| `package_path` | 资产路径或对象目标路径 | 视场景 |

### 5.2 Transform 字段

| 字段 | 格式 | 单位 | 写入 | 读回 | 验证 |
|---|---|---|---|---|---|
| `location` | `[x, y, z]` | cm | ✅ | ✅ | ✅ |
| `rotation` | `[pitch, yaw, roll]` | degrees | ✅ | ✅ | ✅ |
| `relative_scale3d` | `[x, y, z]` | 倍率 | ✅ | ✅ | ✅ |

规则：所有数值使用显式三元数组。不允许使用 `"rotate_left_a_little"` 或用 `"center"` 代替坐标。

### 5.3 Bounds 字段

"大小"不能直接写成 `size`，必须区分语义：

| 字段 | 含义 | 单位 |
|---|---|---|
| `world_bounds_origin` | 世界包围盒中心 | cm |
| `world_bounds_extent` | 世界包围盒半径 | cm |
| `mesh_asset_bounds` | 资源原始包围盒（未经 Transform） | cm |

使用规则：视觉占地用 `world_bounds_extent`；资源原始尺寸用 `mesh_asset_bounds`；不能用 `size` / `actual_size` / `visual_size`。

### 5.4 Collision 字段

| 字段 | 含义 | 适用场景 |
|---|---|---|
| `collision_enabled` | 碰撞启用模式（如 `QueryAndPhysics`） | 全部 |
| `collision_profile_name` | UE5 碰撞预设名（如 `BlockAll`） | 全部 |
| `generate_overlap_events` | 是否生成重叠事件 | 全部 |
| `collision_box_extent` | Box 碰撞体半径（cm） | 仅 BoxComponent |
| `collision_capsule_radius` | Capsule 半径（cm） | 仅 CapsuleComponent |
| `collision_capsule_half_height` | Capsule 半高（cm） | 仅 CapsuleComponent |
| `can_affect_navigation` | 是否影响导航网格 | 全部 |

所有碰撞字段嵌套在 `collision` 对象下。

### 5.5 Navigation 字段

| 字段 | 含义 |
|---|---|
| `can_affect_navigation` | 是否影响导航 |
| `nav_obstacle` | 是否为导航障碍物 |
| `walkable_path_width` | 可行走路径宽度（cm） |

### 5.6 Mesh / Component 字段

| 字段 | 含义 |
|---|---|
| `static_mesh` | 静态网格资产路径 |
| `skeletal_mesh` | 骨骼网格资产路径 |
| `material_overrides` | 材质覆盖列表 |
| `component_name` | 组件名称 |
| `component_class` | 组件类名 |
| `attach_parent` | 父组件名称 |
| `relative_location` / `relative_rotation` / `relative_scale3d` | 组件相对变换 |

### 5.7 Lighting 字段（后续扩展）

以下字段当前不进入首批实现，仅为后续扩展预留定义：

`intensity` / `light_color` / `attenuation_radius` / `source_radius` / `cast_shadows` / `indirect_lighting_intensity`

### 5.8 Camera 字段（后续扩展，截图采集除外）

以下字段仅用于 `capture_viewport_screenshot` 的 camera 参数，不属于核心写操作：

`fov` / `location` / `rotation` / `focus_distance` / `aperture` / `look_at_target_id`

---

## 6. 禁止字段清单

以下字段禁止出现在**执行技术层**（作为字段名或自由文本值）：

`size` / `position` / `center` / `middle` / `big` / `small` / `normal` / `near` / `far` / `slightly_left` / `a_bit_higher` / `a_bit_bigger` / `good_distance` / `nice_spacing` / `proper_scale` / `proper` / `suitable` / `not_too_close` / `large_enough` / `visually correct` / `looks good`

如果设计层或用户层出现这些字段，必须在进入执行层前被转换或拦截。

---

## 7. 默认值与推断规则

默认规则必须写入文档，不允许执行时临时发明。

### 7.1 推荐默认规则

```yaml
defaults:
  size_profile_to_scale:
    small: [0.8, 0.8, 0.8]
    medium: [1.0, 1.0, 1.0]
    large: [1.25, 1.25, 1.25]
    hero_large: [1.6, 1.6, 1.6]

  density_to_spacing:
    sparse: 300.0
    medium: 180.0
    dense: 90.0
```

### 7.2 原则

- 可以推断，但必须声明推断来源
- 推断结果必须可读回
- 推断不能覆盖用户显式值

---

## 8. 缺失与澄清规则

### 8.1 必须触发澄清

- `size_profile` 无法映射到执行值
- "中间"存在多个候选锚点
- 位置关系依赖未定义的 anchor
- 碰撞需要生成但未说明形状
- 存在互相矛盾的约束（如 must_not_overlap + forced_same_location）
- 关卡边界未定义，无法判断 inside/outside

### 8.2 可自动补全

- 已存在统一默认比例表
- 已存在统一 density 间距映射
- 已定义 room_center
- 已定义主路径宽度默认值

---

## 9. 执行后反馈字段规范

任何写操作结束后，都必须输出结构化反馈。

### 9.1 统一反馈外壳

```json
{
  "status": "success | warning | failed | mismatch | validation_error",
  "summary": "...",
  "data": { ... },
  "warnings": [],
  "errors": []
}
```

### 9.2 推荐 data 字段

| 字段 | 含义 |
|---|---|
| `created_objects` | 创建的对象列表 |
| `modified_objects` | 修改的对象列表 |
| `deleted_objects` | 删除的对象列表 |
| `actual_transform` | 写后读回的实际 transform |
| `actual_bounds` | 写后读回的实际 bounds |
| `actual_collision` | 写后读回的实际碰撞状态 |
| `dirty_assets` | 产生的脏资产列表 |
| `validation` | 验证结果 |

### 9.3 反馈示例

```json
{
  "status": "success",
  "summary": "Actor spawned and verified",
  "data": {
    "created_objects": [
      {
        "actor_name": "crate_cluster_a",
        "actor_path": "/Game/Maps/TestMap.TestMap:PersistentLevel.crate_cluster_a"
      }
    ],
    "modified_objects": [],
    "deleted_objects": [],
    "actual_transform": {
      "location": [452.0, -298.0, 0.0],
      "rotation": [0.0, 15.0, 0.0],
      "relative_scale3d": [1.2, 1.2, 1.2]
    },
    "actual_bounds": {
      "world_bounds_origin": [452.0, -298.0, 70.0],
      "world_bounds_extent": [118.0, 79.0, 138.0]
    },
    "dirty_assets": ["/Game/Maps/TestMap"],
    "validation": {
      "inside_playable_area": true,
      "overlaps_main_path": false
    }
  },
  "warnings": [],
  "errors": []
}
```

---

## 10. Agent 执行约束规则

以下为硬约束，不是建议：

### 10.1 执行输入约束

- 执行 Agent 只读取结构化 Spec
- 不直接读取用户自然语言作执行决策

### 10.2 字段约束

- 禁止模糊字段进入执行层（见第 6 节禁止清单）
- 所有 transform / bounds / collision 字段必须显式命名

### 10.3 写后读回约束

- 所有写操作必须自动附带读回结果
- 至少读回 `location` / `rotation` / `relative_scale3d`
- 若涉及碰撞，必须读回 `collision.*`
- 若涉及占地，必须读回 `world_bounds_*`

### 10.4 高风险操作约束

以下操作必须先 dry-run：批量改场景对象、批量改碰撞、批量替换资源、自动生成大规模布局。

---

## 11. v0.1 适用范围

本版适用于：关卡布局、静态物件摆放、简单交互对象、碰撞/导航约束、自动化读回验证、截图采集。

本版暂不深入规范：材质图网络细节、动画蓝图状态机、Niagara 图级节点参数、复杂行为树图编辑、Sequencer 复杂轨道数据。这些计划在 v0.2 / v0.3 扩展。

> 完整的字段→UE5 API 映射关系和 UE5 官方能力总览，参见 `Docs/ue5_capability_map.md`。
