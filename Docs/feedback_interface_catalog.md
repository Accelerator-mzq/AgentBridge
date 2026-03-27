# 反馈接口设计清单

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 设计目标

反馈接口用于解决四件事：

1. 确认当前状态（写前查询）
2. 确认执行结果（写后读回）
3. 确认是否偏离设计（与预期比对）
4. 决定下一步动作（修正或继续）

核心原则：任何 Agent 能改的东西，它都必须也能读。

反馈接口也通过 Bridge 执行（Python / Editor Plugin / Remote Control），不是独立于 Bridge 的子系统。所有间接操作通道均要求 UE5.5.4 Editor 处于运行状态。

---

## 2. 总体设计原则

### 2.1 反馈必须结构化

禁止返回模糊描述（"看起来正常""大概成功了""位置差不多"）。必须返回坐标、旋转、缩放、bounds、路径、脏资源、校验结果、错误码。

### 2.2 反馈必须可比对

反馈字段要能和 Spec 比对。Spec 中有 `location` / `relative_scale3d` / `collision_box_extent`，反馈中也必须返回这些字段的实际值。

### 2.3 写操作必须自动附带读回

写接口自身应返回 `actual_*` 字段（第一次读回），然后再调用独立反馈接口做二次确认（第二次读回）。

### 2.4 反馈必须带稳定标识

至少返回以下之一：`actor_path` / `asset_path` / `level_path` / `component_name`。禁止只返回显示名。

### 2.5 反馈必须分"事实"和"验证"

```yaml
# 事实（读回数据）
actual_transform:
  location: [450, -300, 0]
  rotation: [0, 15, 0]
  relative_scale3d: [1.2, 1.2, 1.2]

# 验证（判定结论）
validation:
  inside_playable_area: true
  overlaps_main_path: false
```

两者不可混在同一层级。

### 2.6 统一响应外壳

所有反馈接口必须返回：

```json
{
  "status": "success | warning | failed | mismatch | validation_error",
  "summary": "...",
  "data": { ... },
  "warnings": [],
  "errors": []
}
```

---

## 3. 结构化验证定义框架

当前验证结果使用 boolean 判定（如 `inside_playable_area: true`）。后续版本应升级为结构化验证：

```yaml
validation:
  status: success    # 或 mismatch
  checks:
    - field: location
      expected: [450.0, -300.0, 0.0]
      actual: [452.0, -298.0, 0.0]
      tolerance: 5.0
      pass: true
    - field: inside_playable_area
      expected: true
      actual: true
      pass: true
```

推荐容差：

| 字段 | 容差 | 说明 |
|---|---|---|
| location | ≤ 0.01 cm | UE5 浮点精度 |
| rotation | ≤ 0.01 degrees | 浮点精度 |
| relative_scale3d | ≤ 0.001 | 浮点精度 |
| world_bounds_extent | ≤ 1.0 cm | 包围盒计算微小差异 |

---

## 4. 核心接口（Phase 1）

以下接口是最先实现的，全部有 Tool Contract 定义和 Schema。

**每个接口的执行能力来自 UE5 官方 API**——Bridge 封装层在 UE5 原生 API 之上增加统一响应格式、错误码和 Schema 对齐。完整的工具→UE5 API 映射表参见 `Docs/ue5_capability_map.md` 第 6 节。

### A1. get_current_project_state

**目的**：返回当前项目与编辑器上下文。

**UE5 能力来源**：`FPaths` + `UKismetSystemLibrary::GetEngineVersion()` + `UEditorLevelLibrary::GetEditorWorld()` | 通道 A/B

**最低返回字段**：
```yaml
data:
  project_name: MyGame
  uproject_path: D:/Proj/MyGame/MyGame.uproject
  engine_version: "5.5.4"
  current_level: /Game/Maps/Warehouse01
  editor_mode: editing
```

**用途**：判断当前项目/地图是否正确，避免在错误上下文执行。

**Tool Contract**：已定义 | **Schema**：已定义

---

### A2. list_level_actors

**目的**：列出关卡中已有 Actor。

**UE5 能力来源**：`UEditorLevelLibrary::GetAllLevelActors()` | 通道 A/B

**最低返回字段**：
```yaml
data:
  actors:
    - actor_name: truck_01
      actor_path: /Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01
      class: /Script/MyGame.BP_DecorTruck_C
```

**用途**：写前判断对象是否存在；写后确认对象已创建。

**Tool Contract**：已定义 | **Schema**：已定义

---

### A3. get_actor_state

**目的**：读取单个 Actor 的核心状态。闭环中最核心的反馈接口。

**UE5 能力来源**：`AActor::GetActorLocation/Rotation/Scale3D()` + `UPrimitiveComponent` 碰撞 API + `AActor::Tags` | 通道 A/B

**最低返回字段**：
```yaml
data:
  actor_name: truck_01
  actor_path: /Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01
  class: /Script/MyGame.BP_DecorTruck_C
  target_level: /Game/Maps/Warehouse01
  transform:
    location: [0.0, 200.0, 0.0]
    rotation: [0.0, 90.0, 0.0]
    relative_scale3d: [1.25, 1.25, 1.25]
  collision:
    collision_profile_name: BlockAll
    collision_enabled: QueryAndPhysics
    generate_overlap_events: false
    collision_box_extent: [210.0, 95.0, 120.0]
    can_affect_navigation: true
  tags: ["Vehicle", "Prop"]
```

**用途**：读回 transform 和 collision 完整状态，支持 spawn_actor / set_actor_transform / set_actor_collision 的写后验证。

**说明**：collision 字段在有碰撞配置时返回完整信息（含 collision_box_extent），使此接口可同时作为 set_actor_collision 的读回验证来源，减少对额外 get_component_state 接口的依赖。

**Tool Contract**：已定义 | **Schema**：已定义

---

### A4. get_actor_bounds

**目的**：读取 Actor 实际世界包围盒。

**UE5 能力来源**：`AActor::GetActorBounds()` → 返回 `FBoxSphereBounds` (origin + extent) | 通道 A/B

**最低返回字段**：
```yaml
data:
  actor_path: /Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01
  world_bounds_origin: [0.0, 200.0, 110.0]
  world_bounds_extent: [210.0, 95.0, 120.0]
```

**用途**：判断视觉占地、越界检查、overlap 预检。

**说明**：data 中 bounds 使用平铺结构（本接口的 data 本身就是 bounds 信息）。写后反馈中 bounds 嵌套在 `actual_bounds` 下。

**Tool Contract**：已定义 | **Schema**：已定义

---

### A5. get_asset_metadata

**目的**：读取资产基础元数据。

**UE5 能力来源**：`UEditorAssetLibrary::DoesAssetExist()` + `FindAssetData()` + `UStaticMesh::GetBoundingBox()` | 通道 A/B

**最低返回字段**：
```yaml
data:
  asset_path: /Game/Props/Crates/SM_Crate_01
  asset_name: SM_Crate_01
  class: StaticMesh
  exists: true
  mesh_asset_bounds:
    world_bounds_origin: [0.0, 0.0, 0.0]
    world_bounds_extent: [50.0, 50.0, 50.0]
```

**用途**：import_assets / create_blueprint_child 的写后验证核心（确认 exists=true）。

**Tool Contract**：已定义 | **Schema**：已定义

---

### A6. get_dirty_assets

**目的**：读取当前未保存脏资产。

**UE5 能力来源**：`UEditorLoadingAndSavingUtils::GetDirtyContentPackages()` / Package 脏状态管理 | 通道 A/B

**最低返回字段**：
```yaml
data:
  dirty_assets:
    - /Game/Maps/Warehouse01
```

**用途**：所有写操作的副作用确认。

**Tool Contract**：已定义 | **Schema**：已定义

---

### A7. get_editor_log_tail

**目的**：读取最近编辑器日志中的警告与错误。

**UE5 能力来源**：`FOutputDevice` / Editor Log 系统 | 通道 A

**最低返回字段**：
```yaml
data:
  lines:
    - "Warning: ..."
    - "Error: ..."
```

**用途**：识别导入/编译/引用失败。

**Tool Contract**：已定义 | **Schema**：暂未单独定义（日志为非结构化文本，可后续补充）

---

### A8. run_map_check

**目的**：运行地图检查并返回结果。

**UE5 能力来源**：Editor 内置 `MAP CHECK` Console Command（UE5 原生地图校验子系统）| 通道 A/C

**最低返回字段**：
```yaml
data:
  level_path: /Game/Maps/Warehouse01
  map_errors: []
  map_warnings:
    - "Actor has invalid lightmap setting"
```

**用途**：地图级健康检查。

**说明**：data 内使用 `map_errors` / `map_warnings`，与外层 `errors` / `warnings`（工具调用级）区分。

**Tool Contract**：已定义 | **Schema**：已定义

---

## 5. 扩展接口（Phase 2）

以下接口在 Phase 2 阶段实现，部分尚无 Tool Contract 定义。

### B1. get_actor_components

**目的**：读取 Actor 组件层级。

**Tool Contract**：待补充 | **Schema**：待补充

---

### B2. get_component_state

**目的**：读取某个组件的具体属性（碰撞 / mesh 偏移 / 组件级 transform）。

**说明**：由于 get_actor_state 已扩展为返回完整 collision 信息，当前对此接口的依赖已降低。如需组件级精细检查，仍建议后续补充。

**Tool Contract**：待补充 | **Schema**：待补充

---

### B3. get_material_assignment

**目的**：读取组件材质槽分配结果。assign_material 的读回验证依赖此接口。

**推荐返回字段**：
```yaml
data:
  actor_path: /Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01
  component_name: TruckMesh
  slots:
    - slot_name: Body
      material_path: /Game/Materials/M_Truck_Rust
```

**Tool Contract**：待补充 | **Schema**：待补充

---

### B4. get_navigation_state

**目的**：读取对象或区域对导航的影响。

**Tool Contract**：待补充 | **Schema**：待补充

---

### B5. validate_actor_inside_bounds

**目的**：检查 Actor 是否在目标边界内。

**推荐返回字段**：
```yaml
data:
  actor_path: ...
  inside_bounds: true
  checked_bounds:
    world_bounds_origin: [0.0, 0.0, 0.0]
    world_bounds_extent: [3000.0, 2500.0, 600.0]
```

**Tool Contract**：已定义 | **Schema**：待补充

---

### B6. validate_actor_non_overlap

**目的**：检查 Actor 是否与其他对象重叠。

**推荐返回字段**：
```yaml
data:
  actor_path: ...
  has_overlap: false
  overlaps: []
```

**Tool Contract**：已定义 | **Schema**：待补充

---

### B7. capture_viewport_screenshot

**目的**：截取视口快照。这是**产物采集**，不做自动判定。截图本身不是验证——它是辅助人工或后续图像对比的素材。

**推荐返回字段**：
```yaml
data:
  image_path: Artifacts/Warehouse01_viewA.png
  camera:
    location: [-600.0, 120.0, 220.0]
    rotation: [-8.0, 20.0, 0.0]
    fov: 60.0
  captured_level: /Game/Maps/Warehouse01
  timestamp: "2026-03-10T12:34:56"
```

**Tool Contract**：已定义 | **Schema**：待补充

**说明**：核心反馈接口（A1-A8）已在 Phase 1 实现，screenshot 通过 CaptureViewportScreenshot 接口已实装。

---

## 5.5 L3 UI 工具接口（Automation Driver）

以下接口通过 UE5 Automation Driver 模拟用户输入，**仅当 L1 语义工具无法覆盖时使用**。优先级最低（L1 > L2 > L3）。每次 L3 操作后，必须通过 L1 接口做独立读回，L3 返回值与 L1 返回值做交叉比对。

### D1. click_detail_panel_button

**目的**：在 Actor 的 Detail Panel 中点击按钮。用于无直接 BlueprintCallable API 的 UI 操作（如 "Add Component" / "Reset to Default" / "Convert to Static Mesh"）。

**Args**：`{ actor_path, button_label, dry_run }`

**执行后端**：默认通过 `start_ui_operation()` / `query_ui_operation()` 异步任务壳调度；底层点击语义由 Automation Driver 提供，避免在 RC 同步链路里直接等待完整 UI 点击

**L1 验证方式**：调用后必须通过 `get_actor_state` 或 `get_component_state` 读回变更

**L3→L1 交叉比对**：L3 返回值（声称操作成功）vs L1 读回值（真实状态）→ 一致=success，不一致=mismatch

**data 关键字段**：`operation` / `executed` / `ui_idle_after` / `duration_seconds` / `tool_layer: "L3_UITool"`

**Tool Contract**：已定义（§7.5.1） | **Category**：`AgentBridge|UITool`

### D2. type_in_detail_panel_field

**目的**：在 Detail Panel 的属性输入框中输入值。用于非反射属性或需要通过 UI 触发 PostEditChangeProperty 的场景。

**Args**：`{ actor_path, property_path, value, dry_run }`

**执行后端**：默认通过异步任务壳调度；定位属性行后直接对可编辑文本控件设值并显式提交

**L1 验证方式**：`get_actor_state` 读回属性值变更

**Tool Contract**：已定义（§7.5.2） | **Category**：`AgentBridge|UITool`

### D3. drag_asset_to_viewport

**目的**：将 Content Browser 中的资产拖拽到 Viewport 指定位置。走 Editor 原生 OnDropped 流程——触发自动碰撞设置、自动命名、自动贴地。L1 `spawn_actor` 不触发这些行为。

**Args**：`{ asset_path, drop_location: [x,y,z], dry_run }`

**执行后端**：默认通过异步任务壳调度；最终放置走 Editor 官方 `DropObjectsAtCoordinates(...)` 路径，而不是裸鼠标拖拽

**L1 验证方式**：`list_level_actors`（确认 Actor 数量增加）+ `get_actor_state`（确认位置接近 drop_location，容差 100cm）

**data 关键字段**：含 `actors_before` / `actors_after` / `actors_created` / `created_actors` 列表

**Tool Contract**：已定义（§7.5.3） | **Category**：`AgentBridge|UITool`

### D4. is_automation_driver_available

**目的**：检查 Automation Driver 模块是否可用。返回 bool，不是 FBridgeResponse。

**使用场景**：Orchestrator 在执行 L3 操作前先检查 Driver 是否可用，不可用时跳过或降级。

### D5. L3 异步任务壳

L3 UI 工具当前的统一执行模型是：

1. `start_ui_operation`：只做参数校验、生成 `operation_id`、入队并立即返回
2. `query_ui_operation`：轮询 `pending / running / success / failed`
3. UI 操作终态后，再用 L1 做独立读回验证

这层异步任务壳的作用是把 Automation Driver 的同步执行从 RC 同步等待链中拆开，降低阻塞风险，并保持 L3→L1 交叉比对闭环。

### L3 使用判定条件（全部满足才允许）

1. L1 无对应 API
2. 操作可结构化描述（路径/名称，非坐标）
3. 结果可通过 L1 工具独立验证
4. 操作可逆或低风险
5. Spec 中显式标注 `execution_method: ui_tool`
6. 已封装为 AgentBridge Subsystem 接口

### L3 容差标准

| 字段 | L1 语义工具容差 | L3 UI 工具容差 | 原因 |
|---|---|---|---|
| location | ≤ 0.01 cm | ≤ 100 cm | UI 拖拽依赖 Viewport 投影精度 + 鼠标释放位置 |

---

## 6. 高级接口（后续扩展）

以下接口为后续扩展方向。

### C1. get_scene_layout_summary

关卡布局摘要（Actor 数量、类型分布、区域密度）。

### C2. get_constraint_evaluation

对 Spec 约束进行统一求值。需要独立约束求解器，当前不实现。

### C3. compare_screenshot_to_baseline

截图对比。可对接 UE5 Screenshot Comparison Tool，当前不实现自动对比。

### C4. get_performance_snapshot

性能快照（FPS / draw calls / primitive count）。

### C5. get_reference_impact

查看操作影响了哪些资源引用。

以上接口均无 Tool Contract 定义和 Schema。

---

## 7. 执行结果反馈结构

这部分不是"查询当前世界"，而是"写操作后工具应返回什么"。

### 7.1 通用写操作反馈外壳

所有写工具必须返回以下结构（与 `write_operation_feedback.response.schema.json` 对齐）：

```yaml
status: success
summary: "..."
data:
  created_objects: []
  modified_objects: []
  deleted_objects: []
  actual_transform: { ... }     # spawn_actor / set_actor_transform 必须返回
  actual_bounds: { ... }        # 可选
  actual_collision: { ... }     # set_actor_collision 返回（Phase 2）
  assigned: { ... }             # assign_material 返回（Phase 2）
  dirty_assets: []
  validation: { ... }
warnings: []
errors: []
```

### 7.2 各写操作的必须返回字段

| 写接口 | 必须返回 |
|---|---|
| import_assets | created_objects（asset_path）+ dirty_assets |
| create_blueprint_child | created_objects（asset_path）+ dirty_assets |
| spawn_actor | created_objects（actor_name + actor_path）+ actual_transform + dirty_assets |
| set_actor_transform | modified_objects + old_transform + actual_transform + dirty_assets |
| set_actor_collision | modified_objects + actual_collision + dirty_assets（Phase 2） |
| assign_material | modified_objects + assigned + dirty_assets（Phase 2） |

---

## 8. 推荐字段命名规范

与字段规范 v0.1 保持一致：

**Transform**：`location` / `rotation` / `relative_scale3d`

**Bounds**：`world_bounds_origin` / `world_bounds_extent` / `mesh_asset_bounds`

**Collision**：`collision_profile_name` / `collision_box_extent` / `collision_capsule_radius` / `collision_capsule_half_height` / `can_affect_navigation`

**Navigation**：`can_affect_navigation` / `nav_obstacle`

**Result**：`actual_transform` / `actual_bounds` / `actual_collision` / `dirty_assets` / `warnings` / `errors`

**禁止使用**：`size` / `actual_size` / `position` / `state_ok` / `looks_good`

---

## 9. 推荐落地顺序

### Phase 1：核心反馈（先实现，"看得见"）

`get_current_project_state` / `list_level_actors` / `get_actor_state` / `get_actor_bounds` / `get_asset_metadata` / `get_dirty_assets` / `get_editor_log_tail` / `run_map_check`

### Phase 2：验证增强（"看得准"）

`validate_actor_inside_bounds` / `validate_actor_non_overlap` / `capture_viewport_screenshot` / `get_component_state` / `get_material_assignment`

### Phase 2.5：L3 UI 工具（"做得广"——覆盖无直接 API 的 UI 操作）

`click_detail_panel_button` / `type_in_detail_panel_field` / `drag_asset_to_viewport` / `is_automation_driver_available`

优先级最低，仅当 L1 语义工具无法覆盖时使用。每次操作后必须通过 L1 做交叉比对验证。

### Phase 3：高级能力（后续扩展，"看得全"）

`get_scene_layout_summary` / `compare_screenshot_to_baseline` / `get_constraint_evaluation`
