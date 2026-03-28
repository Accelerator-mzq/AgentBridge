# Tool Contract v0.1

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 目的

本文档定义 AI Agent 使用的每个工具的契约：参数、返回值、调用前提、风险等级、UE5 依赖与错误处理。

每个工具本质上是对一个或多个 **UE5 官方 API** 的结构化封装。Tool Contract 在 UE5 原生 API 之上增加了参数校验、统一响应格式、错误码、Schema 对齐等编排层价值。所有工具的返回值必须符合 `Schemas/` 中对应的 JSON Schema 定义。

> 完整的 UE5 官方能力总览及工具→API 映射表，参见 `Docs/ue5_capability_map.md`。

---

## 2. 通用规则

### 2.1 标准调用结构

```json
{
  "tool": "tool_name",
  "args": { ... }
}
```

### 2.2 标准响应结构

所有工具必须返回以下外壳：

```json
{
  "status": "success | warning | failed | mismatch | validation_error",
  "summary": "简短机器友好的摘要",
  "data": { ... },
  "warnings": [],
  "errors": []
}
```

### 2.3 status 枚举

| 值 | 含义 |
|---|---|
| `success` | 执行成功，验证通过 |
| `warning` | 执行成功，有非阻塞警告 |
| `failed` | 执行失败 |
| `mismatch` | 执行成功但读回值与预期不符 |
| `validation_error` | 参数/Spec 校验未通过，未执行 |

### 2.4 错误码

```json
{
  "status": "failed",
  "errors": [
    {
      "code": "ASSET_NOT_FOUND",
      "message": "Asset does not exist",
      "details": { "asset_path": "/Game/Props/MissingAsset" }
    }
  ]
}
```

推荐错误码：`INVALID_ARGS` / `ASSET_NOT_FOUND` / `ACTOR_NOT_FOUND` / `LEVEL_NOT_FOUND` / `CLASS_NOT_FOUND` / `PROPERTY_NOT_ALLOWED` / `DRY_RUN_REQUIRED` / `VALIDATION_FAILED` / `WRITE_SCOPE_EXCEEDED` / `EDITOR_NOT_READY` / `BUILD_FAILED` / `SAVE_FAILED` / `READBACK_FAILED`

### 2.5 通用约束

- 禁止隐藏写操作：工具不得修改声明范围之外的对象
- 确定性命名：返回的对象标识必须使用 `actor_path` / `asset_path` / `level_path` 等稳定路径
- 必须支持读回：所有写工具必须返回足够信息用于验证
- 中/高风险写工具应支持 `dry_run: true`

### 2.6 执行通道

每个工具标注了可用的执行通道：

| 通道 | UE5 官方模块 | 说明 |
|---|---|---|
| **A** | Python Editor Scripting (`unreal` 模块) | 进程内调用 |
| **B** | Remote Control API (HTTP REST) | 远程调用，支持 `generateTransaction` |
| **C** | Commandlet / CLI | 无 GUI 批处理 |
| **D** | UAT (BuildCookRun) | 外部构建编排 |

### 2.7 通用数据类型

**Vector3**：`[x, y, z]`（number 数组，长度 3）

**Rotator**：`[pitch, yaw, roll]`（degrees，number 数组，长度 3）

**Transform**：
```json
{
  "location": [0.0, 0.0, 0.0],
  "rotation": [0.0, 0.0, 0.0],
  "relative_scale3d": [1.0, 1.0, 1.0]
}
```

**Bounds**：
```json
{
  "world_bounds_origin": [0.0, 0.0, 0.0],
  "world_bounds_extent": [100.0, 100.0, 100.0]
}
```

---

### 2.6 三层工具优先级

本文档中的工具按确定性和风险等级分为三层。Agent 选择工具时必须按 **L1 > L2 > L3** 优先级：

| 层次 | 名称 | 对应章节 | 优先级 |
|---|---|---|---|
| L1 | 语义工具 | §3（查询）/ §4-5（写入） | 最高（默认主干） |
| L2 | 编辑器服务工具 | §6（验证）/ §7（构建/测试） | 中（工程服务） |
| L3 | UI 工具 | §7.5（Automation Driver） | 最低（仅 fallback） |

L3 使用条件：L1 无对应 API + 操作可结构化 + 结果可通过 L1 验证 + 操作可逆或低风险 + Spec 显式标注 `execution_method: ui_tool` + 已封装为 AgentBridge 接口。

---

## 3. L1 查询工具

### 3.1 get_current_project_state

**目的**：返回当前项目与编辑器上下文。

**UE5 依赖**：`FPaths::GetProjectFilePath()` + `UKismetSystemLibrary::GetEngineVersion()` + `UEditorLevelLibrary::GetEditorWorld()` | 通道 A/B/C（C++ Plugin 推荐）

**Preconditions**：Editor 已启动。

**Args**：
```json
{}
```

**Response data**：
```json
{
  "project_name": "MyGame",
  "uproject_path": "D:/Proj/MyGame/MyGame.uproject",
  "engine_version": "5.5.4",
  "current_level": "/Game/Maps/Warehouse01",
  "editor_mode": "editing"
}
```

**风险等级**：无风险（只读）。

---

### 3.2 list_level_actors

**目的**：列出目标关卡中的 Actor。

**UE5 依赖**：`UEditorLevelLibrary::GetAllLevelActors()` (Editor Scripting Utilities) | 通道 A/B/C（C++ Plugin 推荐）

**Preconditions**：目标关卡已加载。

**Args**：
```json
{
  "level_path": "/Game/Maps/Warehouse01",
  "class_filter": null,
  "tag_filter": null
}
```

**Response data**：
```json
{
  "level_path": "/Game/Maps/Warehouse01",
  "actors": [
    {
      "actor_name": "truck_01",
      "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01",
      "class": "/Script/MyGame.BP_DecorTruck_C"
    }
  ]
}
```

**风险等级**：无风险（只读）。

---

### 3.3 get_actor_state

**目的**：读取单个 Actor 的核心状态（transform / collision / tags）。这是 闭环中最核心的反馈接口。

**UE5 依赖**：`AActor::GetActorLocation/Rotation/Scale3D()` + `UPrimitiveComponent` 碰撞 API + `AActor::Tags` | 通道 A/B/C（C++ Plugin 推荐）

**Preconditions**：Actor 必须存在于已加载关卡中。

**Args**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01"
}
```

**Response data**：
```json
{
  "actor_name": "truck_01",
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01",
  "class": "/Script/MyGame.BP_DecorTruck_C",
  "target_level": "/Game/Maps/Warehouse01",
  "transform": {
    "location": [0.0, 200.0, 0.0],
    "rotation": [0.0, 90.0, 0.0],
    "relative_scale3d": [1.25, 1.25, 1.25]
  },
  "collision": {
    "collision_profile_name": "BlockAll",
    "collision_enabled": "QueryAndPhysics",
    "generate_overlap_events": false,
    "collision_box_extent": [210.0, 95.0, 120.0],
    "can_affect_navigation": true
  },
  "tags": ["Vehicle", "Prop"]
}
```

**风险等级**：无风险（只读）。

**说明**：collision 字段在有碰撞配置时返回完整信息（含 collision_box_extent），使此接口可作为 set_actor_collision 的读回验证来源。

---

### 3.4 get_actor_bounds

**目的**：返回单个 Actor 的世界包围盒。

**UE5 依赖**：`AActor::GetActorBounds()` → 返回 `FBoxSphereBounds` (origin + extent) | 通道 A/B/C（C++ Plugin 推荐）

**Preconditions**：Actor 必须存在。

**Args**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01"
}
```

**Response data**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01",
  "world_bounds_origin": [0.0, 200.0, 110.0],
  "world_bounds_extent": [210.0, 95.0, 120.0]
}
```

**风险等级**：无风险（只读）。

**说明**：data 中 bounds 使用平铺结构（本接口的 data 就是 bounds 信息）。

---

### 3.5 get_asset_metadata

**目的**：返回单个资产的基础元数据。

**UE5 依赖**：`UEditorAssetLibrary::DoesAssetExist()` + `FindAssetData()` + `UStaticMesh::GetBoundingBox()` | 通道 A/B/C（C++ Plugin 推荐）

**Preconditions**：无特殊前提（资产不存在时 exists 返回 false）。

**Args**：
```json
{
  "asset_path": "/Game/Props/Crates/SM_Crate_01"
}
```

**Response data**：
```json
{
  "asset_path": "/Game/Props/Crates/SM_Crate_01",
  "asset_name": "SM_Crate_01",
  "class": "StaticMesh",
  "exists": true,
  "mesh_asset_bounds": {
    "world_bounds_origin": [0.0, 0.0, 0.0],
    "world_bounds_extent": [50.0, 50.0, 50.0]
  }
}
```

**风险等级**：无风险（只读）。

**说明**：mesh_asset_bounds 仅在资产为 StaticMesh 等有几何体的类型时返回。

---

### 3.6 get_dirty_assets

**目的**：返回当前未保存的脏资产列表。

**UE5 依赖**：`UEditorLoadingAndSavingUtils::GetDirtyContentPackages()` 或 Package 脏状态 API | 通道 A/B/C（C++ Plugin 推荐）

**Preconditions**：无。

**Args**：
```json
{}
```

**Response data**：
```json
{
  "dirty_assets": ["/Game/Maps/Warehouse01"]
}
```

**风险等级**：无风险（只读）。

---

### 3.7 get_editor_log_tail

**目的**：返回最近的编辑器日志（警告与错误）。

**UE5 依赖**：`FOutputDevice` / Editor Log 系统 | 通道 A

**Preconditions**：Editor 已启动。

**Args**：
```json
{
  "max_lines": 200
}
```

**Response data**：
```json
{
  "lines": [
    "Warning: ...",
    "Error: ..."
  ]
}
```

**风险等级**：无风险（只读）。

---

## 4. L1 写工具 — 核心

### 4.1 import_assets

**目的**：将外部源文件导入到 Content 目录。

**UE5 依赖**：`UAssetToolsHelpers::GetAssetTools()` + `ImportAssetTasks()` (`FAssetImportTask`) | 通道 A

**Preconditions**：源文件路径可访问；目标路径父目录存在。

**Args**：
```json
{
  "source_dir": "D:/Drops/Props",
  "dest_path": "/Game/Props/Imported",
  "replace_existing": false,
  "dry_run": true
}
```

**Response data（dry_run=true）**：
```json
{
  "would_import": [
    {
      "source_file": "D:/Drops/Props/Crate.fbx",
      "dest_asset_path": "/Game/Props/Imported/Crate"
    }
  ]
}
```

**Response data（dry_run=false）**：
```json
{
  "created_objects": [
    { "asset_path": "/Game/Props/Imported/Crate" }
  ],
  "modified_objects": [],
  "deleted_objects": [],
  "dirty_assets": ["/Game/Props/Imported/Crate"],
  "validation": {}
}
```

**风险等级**：低至中。

**验证闭环**：import_assets → get_asset_metadata（确认 exists=true）→ get_dirty_assets → get_editor_log_tail

---

### 4.2 create_blueprint_child

**目的**：从 C++ 类或 Blueprint 父类创建 Blueprint 子类。

**UE5 依赖**：`UBlueprintFactory` + `UAssetTools::CreateAsset()` | 通道 A

**Preconditions**：父类必须存在且已编译成功。

**Args**：
```json
{
  "parent_class": "/Script/MyGame.MyEnemyBase",
  "package_path": "/Game/Blueprints/Enemies/BP_Enemy_Grunt",
  "dry_run": false
}
```

**Response data**：
```json
{
  "created_objects": [
    { "asset_path": "/Game/Blueprints/Enemies/BP_Enemy_Grunt" }
  ],
  "modified_objects": [],
  "deleted_objects": [],
  "dirty_assets": ["/Game/Blueprints/Enemies/BP_Enemy_Grunt"],
  "validation": {}
}
```

**风险等级**：中。

**验证闭环**：create_blueprint_child → get_asset_metadata（确认 exists=true、class 正确）→ get_dirty_assets

---

### 4.3 spawn_actor

**目的**：在目标关卡中生成 Actor。

**UE5 依赖**：`UEditorLevelLibrary::SpawnActorFromClass()` (Editor Scripting Utilities) + `AActor::SetActorLabel()` + `SetActorScale3D()` | 通道 A/B/C（通道 C: FScopedTransaction 自动 Undo，通道 B: generateTransaction）

**Preconditions**：目标关卡已加载；指定的 class 已存在。

**Args**：
```json
{
  "level_path": "/Game/Maps/Warehouse01",
  "class": "/Script/MyGame.BP_CrateStack_C",
  "actor_name": "crate_cluster_a",
  "transform": {
    "location": [450.0, -300.0, 0.0],
    "rotation": [0.0, 15.0, 0.0],
    "relative_scale3d": [1.2, 1.2, 1.2]
  },
  "dry_run": false
}
```

**Response data**：
```json
{
  "created_objects": [
    {
      "actor_name": "crate_cluster_a",
      "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a"
    }
  ],
  "modified_objects": [],
  "deleted_objects": [],
  "actual_transform": {
    "location": [450.0, -300.0, 0.0],
    "rotation": [0.0, 15.0, 0.0],
    "relative_scale3d": [1.2, 1.2, 1.2]
  },
  "dirty_assets": ["/Game/Maps/Warehouse01"],
  "validation": {}
}
```

**风险等级**：中。必须返回 `actual_transform`。

**验证闭环**：spawn_actor → list_level_actors（确认对象存在）→ get_actor_state（确认 transform）→ get_actor_bounds（确认占地）→ get_dirty_assets

---

### 4.4 set_actor_transform

**目的**：修改现有 Actor 的位置、旋转、缩放。

**UE5 依赖**：`AActor::SetActorLocationAndRotation()` + `AActor::SetActorScale3D()` | 通道 A/B/C（通道 C: FScopedTransaction 自动 Undo，通道 B: generateTransaction）

**Preconditions**：Actor 必须存在于已加载关卡中。

**Args**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a",
  "transform": {
    "location": [460.0, -280.0, 0.0],
    "rotation": [0.0, 30.0, 0.0],
    "relative_scale3d": [1.2, 1.2, 1.2]
  },
  "dry_run": false
}
```

**Response data**：
```json
{
  "created_objects": [],
  "modified_objects": [
    { "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a" }
  ],
  "deleted_objects": [],
  "old_transform": {
    "location": [450.0, -300.0, 0.0],
    "rotation": [0.0, 15.0, 0.0],
    "relative_scale3d": [1.2, 1.2, 1.2]
  },
  "actual_transform": {
    "location": [460.0, -280.0, 0.0],
    "rotation": [0.0, 30.0, 0.0],
    "relative_scale3d": [1.2, 1.2, 1.2]
  },
  "dirty_assets": ["/Game/Maps/Warehouse01"],
  "validation": {}
}
```

**风险等级**：中。必须返回 `old_transform` + `actual_transform`。

**验证闭环**：set_actor_transform → get_actor_state（二次确认 transform）→ get_actor_bounds（确认占地变化）→ get_dirty_assets

---

## 5. L1 写工具 — Phase 2

以下工具在 Phase 2 阶段实现。当前 Tool Contract 给出设计方向，实现时需补充对应反馈接口。

### 5.1 set_actor_collision

**目的**：修改 Actor 的碰撞配置。

**UE5 依赖**：`UPrimitiveComponent::SetCollisionProfileName()` + `SetCollisionEnabled()` + `UBoxComponent::SetBoxExtent()` | 通道 A/B/C（C++ Plugin 推荐）

**Preconditions**：Actor 必须存在；目标碰撞组件必须存在。

**Args**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a",
  "collision": {
    "collision_profile_name": "BlockAll",
    "collision_box_extent": [120.0, 80.0, 140.0],
    "can_affect_navigation": true
  },
  "dry_run": false
}
```

**Response data**：
```json
{
  "created_objects": [],
  "modified_objects": [
    { "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a" }
  ],
  "deleted_objects": [],
  "actual_collision": {
    "collision_profile_name": "BlockAll",
    "collision_box_extent": [120.0, 80.0, 140.0],
    "can_affect_navigation": true
  },
  "dirty_assets": ["/Game/Maps/Warehouse01"],
  "validation": {}
}
```

**风险等级**：中。

**说明**：collision_box_extent 仅适用于 BoxComponent 场景。验证读回依赖 get_actor_state（collision 字段已扩展为完整返回）。

---

### 5.2 assign_material

**目的**：为指定组件和槽位分配材质。

**UE5 依赖**：`UMeshComponent::SetMaterial()` / `UPrimitiveComponent::SetMaterialByName()` | 通道 A/B/C（C++ Plugin 推荐）

**Preconditions**：Actor 和目标组件必须存在；材质资产必须存在。

**Args**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01",
  "component_name": "TruckMesh",
  "slot_name": "Body",
  "material_path": "/Game/Materials/M_Truck_Rust",
  "dry_run": false
}
```

**Response data**：
```json
{
  "created_objects": [],
  "modified_objects": [
    { "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.truck_01" }
  ],
  "deleted_objects": [],
  "assigned": {
    "component_name": "TruckMesh",
    "slot_name": "Body",
    "material_path": "/Game/Materials/M_Truck_Rust"
  },
  "dirty_assets": ["/Game/Maps/Warehouse01"],
  "validation": {}
}
```

**风险等级**：中。

**说明**：验证读回依赖 get_material_assignment（Tool Contract 待补充）。

---

## 6. L2 验证工具

### 6.1 run_map_check

**目的**：运行地图检查。

**UE5 依赖**：Editor 内置 `MAP CHECK` Console Command | 通道 A/C

**Preconditions**：目标关卡已加载。

**Args**：
```json
{
  "level_path": "/Game/Maps/Warehouse01"
}
```

**Response data**：
```json
{
  "level_path": "/Game/Maps/Warehouse01",
  "map_errors": [],
  "map_warnings": ["Actor has invalid lightmap setting"]
}
```

**风险等级**：无风险（只读）。

---

### 6.2 validate_actor_inside_bounds（Phase 2）

**目的**：检查 Actor 是否在指定边界内。

**UE5 依赖**：`AActor::GetActorBounds()` 获取 Actor bounds → 自建 AABB 包含判定逻辑（UE5 无直接 "is inside bounds" 单一 API；可选用 `FBox::IsInside()` 做几何判定） | 通道 A/B/C（C++ Plugin 推荐）

**Args**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a",
  "bounds": {
    "world_bounds_origin": [0.0, 0.0, 0.0],
    "world_bounds_extent": [3000.0, 2500.0, 600.0]
  }
}
```

**Response data**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a",
  "inside_bounds": true,
  "checked_bounds": {
    "world_bounds_origin": [0.0, 0.0, 0.0],
    "world_bounds_extent": [3000.0, 2500.0, 600.0]
  }
}
```

---

### 6.3 validate_actor_non_overlap（Phase 2）

**目的**：检查 Actor 是否与其他对象重叠。

**UE5 依赖**：`UWorld::OverlapMultiByChannel()` 或 `UPrimitiveComponent::GetOverlappingActors()`（UE5 原生碰撞检测 API）| 通道 A/B/C（C++ Plugin 推荐）

**Args**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a",
  "exclude_actor_paths": [],
  "check_blocking_only": true
}
```

**Response data**：
```json
{
  "actor_path": "/Game/Maps/Warehouse01.Warehouse01:PersistentLevel.crate_cluster_a",
  "has_overlap": false,
  "overlaps": []
}
```

---

### 6.4 capture_viewport_screenshot

**目的**：截取视口快照用于辅助检查。这是产物采集，不做自动判定。UE5 内置 Screenshot Comparison Tool 已通过 CaptureViewportScreenshot 接口实装，后续可对接 Screenshot Comparison Tool 实现自动对比。

**UE5 依赖**：`FViewport::ReadPixels()` + `FImageUtils::PNGCompressImageArray()` + `FFileHelper::SaveArrayToFile()` | 通道 C

**Args**：
```json
{
  "screenshot_name": "Warehouse01_viewA"
}
```

**Response data**：
```json
{
  "screenshot_name": "Warehouse01_viewA",
  "output_path": "Saved/Automation/Screenshots/Warehouse01_viewA.png",
  "file_exists": true,
  "file_size": 356214.0,
  "waited_ms": 36.7
}
```

---

## 7. L2 构建 / 测试工具

### 7.1 build_project

**目的**：构建 Editor 或 Game Target。

**UE5 依赖**：**UAT `BuildCookRun` / `BuildTarget`**（C# 外部程序，通过 `RunUAT.bat` 调用）| 通道 D（Editor 外部执行）

**注意**：此工具不在 Editor 内部执行——它通过 Bridge 层调用外部的 UAT 进程。Bridge 需要具备"启动外部进程并等待结果"的能力。

**Args**：
```json
{
  "target": "MyGameEditor",
  "configuration": "Development",
  "platform": "Win64"
}
```

**Response data**：
```json
{
  "build_success": true,
  "build_log_path": "Artifacts/build_editor.log"
}
```

---

### 7.2 run_automation_tests

**目的**：运行 UE5 自动化测试。

**UE5 依赖**：**UE5 Automation Test Framework**（`FAutomationTestBase`）——通过 Console Command `Automation RunTests <Filter>` 触发 | 通道 A/C/D

**说明**：`filter` 参数匹配 UE5 Automation Test Framework 中注册的测试层级名称（如 `"Project.Bridge.SpawnActor"`）。也可用 `RunFilter Smoke` 按 Test Flag 过滤。在 CI/CD 中可通过 Commandlet（通道 C）或 UAT `BuildCookRun -run -editortest -RunAutomationTest=<Filter>`（通道 D）无人值守执行。当前 UE5.5.4 项目口径中，不再把旧的 `RunUAT RunAutomationTests ...` 作为通过标准。

**Args**：
```json
{
  "filter": "Project.Smoke",
  "report_path": "Artifacts/automation_report.json"
}
```

**Response data**：
```json
{
  "passed": 12,
  "failed": 0,
  "report_path": "Artifacts/automation_report.json"
}
```

---

### 7.3 save_named_assets

**目的**：保存指定的脏资产（有边界范围，优先于 save_all）。

**UE5 依赖**：`UEditorAssetLibrary::SaveLoadedAssets()` / `UEditorLoadingAndSavingUtils::SavePackages()` | 通道 A/B/C（C++ Plugin 推荐）

**Args**：
```json
{
  "asset_paths": ["/Game/Maps/Warehouse01"]
}
```

**Response data**：
```json
{
  "saved_assets": ["/Game/Maps/Warehouse01"],
  "failed_assets": []
}
```

---

### 7.4 undo_last_transaction

**目的**：远程触发 UE5 原生 Undo，形成“写入 -> 回滚 -> 读回验证”的自动闭环。

**UE5 依赖**：`GEditor->UndoTransaction()`（Editor Undo 栈）| 通道 C（C++ Plugin）

**Preconditions**：Editor 已就绪；Undo 栈中存在可撤销事务。

**Args**：
```json
{
  "steps": 1
}
```

**Response data**：
```json
{
  "requested_steps": 1,
  "undone_steps": 1,
  "fully_undone": true
}
```

**状态语义**：
- `success`：至少成功撤销 1 步
- `warning`：Undo 栈为空（`undone_steps=0`）
- `validation_error`：`steps <= 0`

**风险等级**：中。会直接修改当前编辑器状态，建议与读回验证成对使用。

---

## 7.5 L3 UI 工具（Automation Driver 执行后端）

优先级最低——仅当 L1 语义工具无法覆盖时使用。每次 L3 操作后必须通过 L1 做独立读回，L3 返回值与 L1 返回值交叉比对。

### 7.5.1 click_detail_panel_button

**层次**：L3 UI 工具

**目的**：在 Actor 的 Detail Panel 中点击按钮。用于无直接 BlueprintCallable API 的 Detail Panel 按钮操作（如 "Add Component" / "Reset to Default" / "Convert to Static Mesh"）。

**Args**：`{ "actor_path": string, "button_label": string, "dry_run": bool }`

**UE5 依赖**：统一通过 `start_ui_operation()` / `query_ui_operation()` 异步任务壳调度；底层点击语义由 Automation Driver 提供，避免在 RC 同步调用链里直接等待完整 UI 点击 | 通道 C 专用

**验证方式**（调用后必须执行）：L1 `get_actor_state` 或 `get_component_state` 读回变更

**风险等级**：中（UI 操作确定性低于 L1，但可通过 L1 读回验证）

**data（L3 返回值）**：
```json
{
  "operation": "ClickDetailPanelButton",
  "executed": true,
  "ui_idle_after": true,
  "duration_seconds": 1.2,
  "tool_layer": "L3_UITool",
  "actor_path": "/Game/Maps/Demo.Chair_01",
  "button_label": "Add Component"
}
```

### 7.5.2 type_in_detail_panel_field

**层次**：L3 UI 工具

**目的**：在 Detail Panel 的属性输入框中输入值。用于非反射属性或需要通过 UI 触发 PostEditChangeProperty 的场景。

**Args**：`{ "actor_path": string, "property_path": string, "value": string, "dry_run": bool }`

**UE5 依赖**：统一通过 `start_ui_operation()` / `query_ui_operation()` 异步任务壳调度；定位属性行后对可编辑文本控件直接设值并显式提交，避免单纯依赖裸键盘序列 | 通道 C 专用

**验证方式**：L1 `get_actor_state` 读回属性值变更

**风险等级**：中

### 7.5.3 drag_asset_to_viewport

**层次**：L3 UI 工具

**目的**：将 Content Browser 中的资产拖拽到 Viewport 指定位置。走 Editor 原生 OnDropped 流程——触发自动碰撞设置、自动命名、自动贴地。L1 `spawn_actor` 不触发这些行为。

**Args**：`{ "asset_path": string, "drop_location": [x, y, z], "dry_run": bool }`

**UE5 依赖**：统一通过 `start_ui_operation()` / `query_ui_operation()` 异步任务壳调度；最终放置走 Editor 官方 `DropObjectsAtCoordinates(...)` 路径，而不是裸鼠标拖拽 | 通道 C 专用

**验证方式**：L1 `list_level_actors`（确认 Actor 数量增加）+ L1 `get_actor_state`（确认位置接近 drop_location，容差 100cm）

**风险等级**：中

**data（L3 返回值）**：
```json
{
  "operation": "DragAssetToViewport",
  "executed": true,
  "ui_idle_after": true,
  "duration_seconds": 3.5,
  "tool_layer": "L3_UITool",
  "asset_path": "/Game/Meshes/SM_Chair",
  "drop_location": [800.0, 600.0, 0.0],
  "actors_before": 10,
  "actors_after": 11,
  "actors_created": 1,
  "created_actors": [
    { "actor_name": "SM_Chair_1", "actor_path": "/Game/Maps/Demo.SM_Chair_1" }
  ]
}
```

### 7.5.4 L3→L1 交叉比对

每次 L3 操作后，通过 `CrossVerifyUIOperation(L3Response, L1VerifyFunc, L1VerifyParams)` 做交叉比对：

1. L3 返回值（UI 工具声称的执行结果）
2. L1 独立读回（语义工具看到的真实状态）
3. 比对两者：一致 → `success`，不一致 → `mismatch`（含字段级差异列表）

返回 `FBridgeUIVerification`：
```json
{
  "final_status": "success",
  "consistent": true,
  "mismatches": []
}
```

### 7.5.5 L3 异步任务壳

为兼容 UE5.5.4 下 Automation Driver 同步 API 的线程约束，L3 UI 工具统一补了一层异步任务壳：

1. `start_ui_operation`：只负责参数校验、生成 `operation_id`、入队并立即返回
2. `query_ui_operation`：轮询 `pending / running / success / failed`
3. UI 操作终态后，必须继续通过 L1 做独立读回验证

当前工程中的默认口径是：

- `click_detail_panel_button`：默认包装函数已切到异步任务壳
- `type_in_detail_panel_field`：已接入异步任务壳
- `drag_asset_to_viewport`：已接入异步任务壳

因此，L3 的统一策略是“异步调度 + L1 读回验证”，而不是在 RC 同步请求里直接等待完整 UI 操作结束。

---

## 8. 风险策略

| 等级 | 要求 | 适用工具 |
|---|---|---|
| 无风险 | 可直接调用 | 所有 L1 查询工具、run_map_check、capture_viewport_screenshot |
| 低风险 | 须记录日志 | import_assets |
| 中风险 | 须支持 dry-run + 读回 + 变更报告 | spawn_actor / set_actor_transform / create_blueprint_child / set_actor_collision / assign_material |
| 中风险 | 须 dry-run + L1 读回 + **L3→L1 交叉比对** | L3 全部接口（click_detail_panel_button / type_in_detail_panel_field / drag_asset_to_viewport） |
| 高风险 | 须 dry-run + 确认 + checkpoint（后续扩展） | delete_assets / bulk_replace_references / save_all_dirty_assets 等 |

---

## 9. 命名规则

工具名应满足：动词优先、语义具体、单一职责。

正确示例：`spawn_actor` / `get_actor_state` / `run_map_check`

错误示例：`do_scene_change` / `fix_everything` / `modify_object`

---

## 10. 版本演进

当前版本为 v0.1。后续版本可增加：

- Blueprint 图安全编辑契约
- Niagara / Sequencer 契约
- 批量场景布局契约
- rollback / checkpoint 契约

应尽量保持向后兼容。

