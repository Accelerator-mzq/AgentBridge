# Bridge 封装层实现方案

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 文档目的

本文档定义 Bridge 封装层的实现方案：**C++ Editor Plugin 为核心**，Python 和 Remote Control API 为客户端通道。

**v0.2 → v0.3 核心变更**：Bridge 核心实现从 Python 脚本升级为 C++ Editor Plugin（`AgentBridge Plugin`）。在本次回滚闭环增强后新增 `undo_last_transaction` 接口。全部接口在 `UAgentBridgeSubsystem` 中以 C++ 原生代码实现，Python 层为轻量客户端，通过 Remote Control API 调用 C++ Plugin 暴露的接口（具体以第 5 章接口总表为准）。

**核心定位不变**：Bridge 封装层的全部执行能力来自 UE5 官方 API。我们不创造新的引擎 API——C++ Plugin 中的每个函数都是对 UE5 官方 API 的结构化封装。

---

## 2. 三通道架构

### 2.1 架构概述

```
Agent / Orchestrator / CLI
    │
    ├── 通道 A: Python Editor Scripting
    │   import unreal → unreal.get_editor_subsystem(AgentBridgeSubsystem)
    │   进程内，快速原型
    │
    ├── 通道 B: Remote Control API（直接调用 UE5 原生 API）
    │   HTTP PUT :30010/remote/object/call
    │   兼容模式（无 Plugin 时 fallback）
    │
    └── 通道 C: C++ Plugin（推荐）
        HTTP PUT :30010/remote/object/call → AgentBridgeSubsystem
        C++ 端完成参数校验 + FScopedTransaction + 写后读回
        Python 端收到完整的 FBridgeResponse JSON
    │
    ▼
AgentBridge C++ Plugin (UAgentBridgeSubsystem)
    │  参数校验 → UE5 API 调用 → 写后读回 → 统一响应构造
    ▼
UE5.5.4 Editor
```

### 2.2 通道对比

| 维度 | 通道 A | 通道 B | 通道 C（推荐） |
|---|---|---|---|
| 调用方式 | `import unreal` | HTTP PUT 原生 API | HTTP PUT → C++ Plugin |
| 运行位置 | Editor 进程内 Python | Editor 进程外 HTTP | Editor 进程外 HTTP → 进程内 C++ |
| 参数校验 | Python 端 | Python 端 | **C++ 端**（`ValidateTransform` 等） |
| Transaction | 需手动 | `generateTransaction` | **FScopedTransaction**（自动） |
| 写后读回 | Python 端实现 | Python 端实现 | **C++ 端实现**（`FBridgeTransform::FromActor`） |
| 响应构造 | Python `make_response` | Python `make_response` | **C++ `FBridgeResponse`** |
| 性能 | 中 | 低 | **最高**（C++ 原生） |
| 适用场景 | 快速原型 | 无 Plugin 时 fallback | **生产环境推荐** |

### 2.3 通道选择策略

```python
from bridge_core import BridgeChannel, set_channel

# 推荐：C++ Plugin 已安装时
set_channel(BridgeChannel.CPP_PLUGIN)

# 快速原型 / 无 Plugin
set_channel(BridgeChannel.PYTHON_EDITOR)

# 兼容模式 / 无 Plugin + 进程外
set_channel(BridgeChannel.REMOTE_CONTROL)

# 开发调试
set_channel(BridgeChannel.MOCK)
```

---

## 3. C++ Plugin 代码结构

### 3.1 Plugin 目录布局

```
Plugins/
├── AgentBridge/                          ← 功能 Plugin
│   ├── AgentBridge.uplugin
│   └── Source/AgentBridge/
│       ├── AgentBridge.Build.cs          ← 构建配置（模块依赖）
│       ├── Public/
│       │   ├── BridgeTypes.h             ← 响应结构体 / 错误码 / 枚举
│       │   ├── AgentBridgeSubsystem.h    ← 核心 Subsystem 头文件（接口声明 + L3 交叉比对）
│       │   ├── AgentBridgeCommandlet.h   ← Commandlet 无头执行
│       │   └── UATRunner.h              ← UAT 子进程封装
│       └── Private/
│           ├── AgentBridgeSubsystem.cpp  ← 全部接口实现（~960 行）
│           ├── AgentBridgeModule.cpp     ← 模块注册
│           ├── AgentBridgeCommandlet.cpp ← Commandlet 实现
│           └── UATRunner.cpp            ← UAT 实现
│
└── AgentBridgeTests/                     ← 测试 Plugin
    ├── AgentBridgeTests.uplugin
    └── Source/AgentBridgeTests/
        ├── AgentBridgeTests.Build.cs
        └── Private/
            ├── AgentBridgeTestsModule.cpp
            ├── L1_QueryTests.cpp         ← 7 个 Simple Automation Test
            ├── L1_WriteTests.cpp         ← 4 个 Simple Automation Test
            ├── L2_ClosedLoopSpecs.spec.cpp ← 3 个 Automation Spec
            ├── L3_FunctionalTestActor.h/.cpp ← Functional Test
            └── AgentBridgeGauntletController.h/.cpp ← Gauntlet Controller
```

### 3.2 核心类职责

| 类 | 父类 | 职责 |
|---|---|---|
| `UAgentBridgeSubsystem` | `UEditorSubsystem` | 全部 Bridge 接口（L1 + L2 + L3） + 参数校验 + Transaction |
| `FBridgeResponse` | `USTRUCT` | 统一响应外壳 + JSON 序列化 |
| `FBridgeTransform` | `USTRUCT` | Transform 结构 + 读取 + 容差比对 |
| `EBridgeStatus` | `UENUM` | 5 值状态枚举 |
| `EBridgeErrorCode` | `UENUM` | 错误码枚举 |
| `UAgentBridgeCommandlet` | `UCommandlet` | 无头执行（Spec / 测试 / 单工具） |
| `FUATRunner` | — | UAT 子进程封装（BuildCookRun / RunTests / Gauntlet） |

---

## 4. 统一响应体系

### 4.1 C++ 端（BridgeTypes.h）

C++ 端是响应的"权威实现"——Python 端的 `make_response` 是 C++ `FBridgeResponse` 的镜像。

```cpp
// 构造成功响应
FBridgeResponse Response = AgentBridge::MakeSuccess(TEXT("Actor state fetched"), Data);

// 构造失败响应
FBridgeResponse Response = AgentBridge::MakeFailed(
    TEXT("Actor not found"),
    EBridgeErrorCode::ActorNotFound,
    FString::Printf(TEXT("No actor at: %s"), *ActorPath));

// 构造参数校验错误
FBridgeResponse Response = AgentBridge::MakeValidationError(
    TEXT("ActorPath"), TEXT("must be non-empty"));

// 序列化为 JSON（返回给 RC API / Python 客户端）
FString JsonString = Response.ToJsonString();
```

### 4.2 C++ → Python 映射

| C++ (BridgeTypes.h) | Python (bridge_core.py) |
|---|---|
| `EBridgeStatus::Success` | `"success"` |
| `EBridgeStatus::ValidationError` | `"validation_error"` |
| `FBridgeError(EBridgeErrorCode::ActorNotFound, msg)` | `make_error("ACTOR_NOT_FOUND", msg)` |
| `AgentBridge::MakeSuccess(summary, data)` | `make_response("success", summary, data)` |
| `FBridgeResponse::ToJsonString()` | `json.dumps(response_dict)` |
| `FBridgeTransform::NearlyEquals(other, tol)` | `abs(a - b) <= tolerance`（v0.2 verifier.py） |
| `FScopedTransaction` | `generateTransaction: true`（通道 B） |

---

## 5. 工具接口总表

全部接口在 `UAgentBridgeSubsystem` 中实现为 `UFUNCTION(BlueprintCallable)`，自动通过 Remote Control API 暴露。按三层分类：

### 5.1 L1 查询接口（7 个语义工具）

| 接口 | C++ 方法 | UE5 API | Schema |
|---|---|---|---|
| get_current_project_state | `GetCurrentProjectState()` | FPaths + FApp::GetBuildVersion | `feedback/project/` |
| list_level_actors | `ListLevelActors(ClassFilter)` | EditorLevelLibrary::GetAllLevelActors | `feedback/level/` |
| get_actor_state | `GetActorState(ActorPath)` | AActor Transform/Collision/Tags | `feedback/actor/` |
| get_actor_bounds | `GetActorBounds(ActorPath)` | AActor::GetActorBounds | `feedback/actor/` |
| get_asset_metadata | `GetAssetMetadata(AssetPath)` | EditorAssetLibrary + StaticMesh | `feedback/asset/` |
| get_dirty_assets | `GetDirtyAssets()` | FEditorFileUtils::GetDirtyContentPackages | `feedback/asset/` |
| run_map_check | `RunMapCheck()` | Console Command "MAP CHECK" | `feedback/validation/` |

### 5.2 L1 写接口（4 个语义工具，全部 FScopedTransaction）

| 接口 | C++ 方法 | UE5 API | Transaction |
|---|---|---|---|
| spawn_actor | `SpawnActor(Level, Class, Name, Transform, DryRun)` | EditorLevelLibrary::SpawnActorFromClass | ✅ FScopedTransaction |
| set_actor_transform | `SetActorTransform(ActorPath, Transform, DryRun)` | AActor::SetActorLocationAndRotation | ✅ FScopedTransaction |
| import_assets | `ImportAssets(SourceDir, DestPath, Replace, DryRun)` | IAssetTools::ImportAssetTasks | ✅ FScopedTransaction |
| create_blueprint_child | `CreateBlueprintChild(ParentClass, PackagePath, DryRun)` | UBlueprintFactory + CreateAsset | ✅ FScopedTransaction |

### 5.3 L2 验证接口（3 个编辑器服务工具）

| 接口 | C++ 方法 | UE5 API |
|---|---|---|
| validate_actor_inside_bounds | `ValidateActorInsideBounds(ActorPath, Origin, Extent)` | FBox::IsInside |
| validate_actor_non_overlap | `ValidateActorNonOverlap(ActorPath)` | UWorld::OverlapMultiByChannel |
| run_automation_tests | `RunAutomationTests(Filter, ReportPath)` | Automation Test Framework |

### 5.4 L2 构建接口（1 个编辑器服务工具）

| 接口 | C++ 方法 | UE5 模块 |
|---|---|---|
| build_project | `BuildProject(Platform, Configuration, DryRun)` | UAT BuildCookRun（FUATRunner） |

### 5.5 L2 辅助接口（3 个编辑器服务工具）

| 接口 | C++ 方法 | UE5 API |
|---|---|---|
| save_named_assets | `SaveNamedAssets(AssetPaths)` | UEditorAssetLibrary::SaveAsset / FEditorFileUtils::SaveDirtyPackages |
| capture_viewport_screenshot | `CaptureViewportScreenshot(ScreenshotName)` | FViewport::ReadPixels + FImageUtils::PNGCompressImageArray + FFileHelper::SaveArrayToFile（同步写出，返回即已落盘） |
| undo_last_transaction | `UndoLastTransaction(Steps)` | GEditor->UndoTransaction |

### 5.6 L3 UI 工具接口（3 个 + 1 辅助，Automation Driver 执行后端）

仅当 L1 语义工具无法覆盖时使用。每次 L3 操作后必须通过 L1 工具做独立读回，L3 返回值与 L1 返回值交叉比对。

| 接口 | C++ 方法 | Automation Driver 用法 | L1 验证方式 |
|---|---|---|---|
| click_detail_panel_button | `ClickDetailPanelButton(ActorPath, ButtonLabel, DryRun)` | FindElement(By::Text) → Click | GetActorState / GetComponentState |
| type_in_detail_panel_field | `TypeInDetailPanelField(ActorPath, PropertyPath, Value, DryRun)` | FindElement → Click → Ctrl+A → Type → Enter | GetActorState |
| drag_asset_to_viewport | `DragAssetToViewport(AssetPath, DropLocation, DryRun)` | FindElement → Press → MoveTo → Release | ListLevelActors + GetActorState |
| is_automation_driver_available | `IsAutomationDriverAvailable()` | IsModuleLoaded("AutomationDriver") | — |

Category 标籤：`AgentBridge|UITool`（与 L1 的 `Query` / `Write` 和 L2 的 `Validate` / `Build` / `Utility` 区分）

---

## 6. Transaction 管理

### 6.1 FScopedTransaction（通道 C 核心优势）

C++ Plugin 中的全部写接口使用 `FScopedTransaction`：

```cpp
FBridgeResponse UAgentBridgeSubsystem::SpawnActor(...)
{
    // ... 参数校验 ...

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("AgentBridge: Spawn %s"), *ActorName)));

    AActor* NewActor = UEditorLevelLibrary::SpawnActorFromClass(Class, Location, Rotation);
    NewActor->SetActorLabel(ActorName);
    NewActor->SetActorScale3D(Scale);

    // FScopedTransaction 作用域结束时自动提交
    // 异常时自动回滚
    // Ctrl+Z 可撤销
}
```

### 6.2 三通道的 Transaction 对比

| 通道 | Transaction 机制 | Undo 方式 |
|---|---|---|
| 通道 A (Python) | 需手动 `GEditor->BeginTransaction()` | 手动 |
| 通道 B (RC API) | `generateTransaction: true` | HTTP 参数 |
| **通道 C (C++ Plugin)** | **FScopedTransaction（自动）** | **Ctrl+Z / GEditor->UndoTransaction()** |

---

## 7. Python 客户端层

### 7.1 Python 在 v0.3 中的角色

Python 从"Bridge 核心实现"变为"C++ Plugin 的轻量客户端"：

```
v0.2: Python 实现全部逻辑 → 调用 UE5 API
v0.3: Python 调用 C++ Plugin → Plugin 实现全部逻辑 → 调用 UE5 API
```

### 7.2 Python 代码结构

```
Scripts/bridge/
├── bridge_core.py              ← 通道切换 + call_cpp_plugin() + 统一响应（Mock/fallback）
├── remote_control_client.py    ← HTTP 客户端（通道 B/C 共用）
├── query_tools.py              ← 三通道分发：Python / RC API / C++ Plugin
├── write_tools.py              ← 三通道分发 + Transform 格式转换
├── ue_helpers.py               ← 通道 A 专用辅助（保留用于快速原型）
└── uat_runner.py               ← UAT Python 封装（CLI / Orchestrator 使用）
```

### 7.3 通道 C 调用流程

```python
# bridge_core.py
def call_cpp_plugin(function_name, parameters=None):
    """通过 RC API 调用 C++ AgentBridge Subsystem。"""
    from remote_control_client import call_function
    subsystem_path = "/Script/AgentBridge.Default__AgentBridgeSubsystem"
    return call_function(subsystem_path, function_name, parameters)

# query_tools.py
def get_actor_state(actor_path):
    channel = get_channel()
    if channel == BridgeChannel.CPP_PLUGIN:
        return call_cpp_plugin("GetActorState", {"ActorPath": actor_path})
    elif channel == BridgeChannel.PYTHON_EDITOR:
        return _get_actor_state_python(actor_path)
    elif channel == BridgeChannel.REMOTE_CONTROL:
        return _get_actor_state_rc(actor_path)
```

---

## 8. 执行模式

### 8.1 四种执行模式

| 模式 | 入口 | UE5 官方模块 | 适用场景 |
|---|---|---|---|
| 交互式 | Editor UI / Python 控制台 | Python Editor Scripting | 开发调试 |
| 远程调用 | HTTP PUT / MCP / Python CLI | Remote Control API | Agent 集成 |
| 无头批处理 | `UE5Editor-Cmd -run=AgentBridge` | Commandlet | CI/CD Spec 执行 |
| 构建自动化 | `RunUAT BuildCookRun` | UAT | 项目构建 |

### 8.2 Commandlet 执行

```bash
# 执行 Spec
UE5Editor-Cmd.exe MyGame.uproject -run=AgentBridge \
    -Spec="AgentSpecs/warehouse.yaml" \
    -Report="Artifacts/report.json" -Unattended -NoPause -NullRHI

# 运行测试
UE5Editor-Cmd.exe MyGame.uproject -run=AgentBridge \
    -RunTests="Project.AgentBridge.L1" -Unattended -NoPause -NullRHI

# 单工具
UE5Editor-Cmd.exe MyGame.uproject -run=AgentBridge \
    -Tool="GetDirtyAssets" -Unattended -NoPause -NullRHI
```

### 8.3 UAT / Gauntlet 执行

```bash
# UAT 构建
RunUAT.bat BuildCookRun -project=MyGame.uproject -platform=Win64 -build -cook -stage -pak

# UAT 运行测试（UE5.5 推荐）
RunUAT.bat BuildCookRun -project=MyGame.uproject -run -editortest -RunAutomationTest=Project.AgentBridge

# Gauntlet 完整 CI/CD
RunUAT.bat RunGauntlet -project=MyGame.uproject -Test=AgentBridge.AllTests
```

---

## 9. 与 Schema 的对应关系

C++ Plugin 的返回值通过 `FBridgeResponse::ToJsonString()` 序列化后，必须符合 Schemas/ 中定义的 JSON Schema。

| C++ 方法 | Schema 文件 | Transaction |
|---|---|---|
| `GetCurrentProjectState()` | `feedback/project/get_current_project_state.response.schema.json` | — |
| `ListLevelActors()` | `feedback/level/list_level_actors.response.schema.json` | — |
| `GetActorState()` | `feedback/actor/get_actor_state.response.schema.json` | — |
| `GetActorBounds()` | `feedback/actor/get_actor_bounds.response.schema.json` | — |
| `GetAssetMetadata()` | `feedback/asset/get_asset_metadata.response.schema.json` | — |
| `GetDirtyAssets()` | `feedback/asset/get_dirty_assets.response.schema.json` | — |
| `RunMapCheck()` | `feedback/validation/run_map_check.response.schema.json` | — |
| `SpawnActor()` | `write_feedback/write_operation_feedback.response.schema.json` | ✅ FScopedTransaction |
| `SetActorTransform()` | 同上 | ✅ FScopedTransaction |
| `ImportAssets()` | 同上 | ✅ FScopedTransaction |
| `CreateBlueprintChild()` | 同上 | ✅ FScopedTransaction |

**Schema 是编排层自有契约**——UE5 没有 JSON Schema 体系。Schema 校验（`validate_examples.py`）永久保留在 Python 端。

---

## 10. 开发步骤建议

### 阶段 1：C++ Plugin 核心（第 2-3 周）

1. 创建 AgentBridge Plugin 骨架（.uplugin + Build.cs + Module.cpp）
2. 实现 `BridgeTypes.h`（响应结构体 + 枚举 + 辅助函数）
3. 实现 `UAgentBridgeSubsystem` 查询接口（7 个）
4. 验证 Remote Control API 自动暴露（curl 可调用）
5. 实现 `UAgentBridgeSubsystem` 写接口（4 个 + FScopedTransaction）
6. 验证 Undo：SpawnActor → `UndoLastTransaction(1)`（或 Ctrl+Z）→ Actor 消失

### 阶段 2：测试 + Commandlet（第 4-5 周）

1. 创建 AgentBridgeTests Plugin
2. 实现 L1 测试（11 个 Simple Automation Test）
3. 实现 L2 测试（3 个 Automation Spec）
4. 实现 `UAgentBridgeCommandlet`
5. 验证无头执行：`-run=AgentBridge -Tool=ListLevelActors`
6. 实现 `FUATRunner` + `BuildProject` 接口

### 阶段 3：L3 + Gauntlet + Python 客户端（第 6-8 周）

1. 创建 FTEST_ 测试地图 + L3 FunctionalTestActor
2. 实现 GauntletTestController
3. 编写 Gauntlet C# TestConfig
4. 更新 Python 层（`call_cpp_plugin` + 三通道分发）
5. 验证 Gauntlet CI/CD：`RunUAT RunGauntlet -Test=AgentBridge.SmokeTests`

---

## 11. 已知限制与注意事项

### 11.1 C++ Plugin 编译要求

- 需要 UE5 源码或已安装的 Editor with C++ 编译支持
- 首次编译需要 Visual Studio 2022 / Xcode / clang
- Plugin 代码变更需重启 Editor（Hot Reload 对 Editor Plugin 不完全可靠）

### 11.2 Remote Control API 注意事项

- `FBridgeResponse` 是 USTRUCT，RC API 通过反射自动序列化
- 属性名使用 PascalCase（UE5 C++ 风格），Python 端负责 PascalCase↔snake_case 转换
- 默认端口 30010，可在 Project Settings → Remote Control 中修改

### 11.3 Commandlet 中的 YAML 解析

UE5 C++ 没有内置 YAML 解析器。Commandlet 执行 Spec 时通过 `IPythonScriptPlugin::ExecPythonCommand()` 调用 Python Orchestrator 完成 YAML 解析。这是 C++ 和 Python 的合理分工——C++ 处理引擎操作，Python 处理文本解析。

### 11.4 Blueprint 图深改限制

`K2Node`（Blueprint 图节点）的创建/删除/连接不是 BlueprintCallable 的。这个限制来自 UE5 官方——C++ Plugin 也无法通过 L1 语义工具绕过。L3 UI 工具（Automation Driver）未来可通过模拟拖拽覆盖简单的节点连接操作，但复杂的 Blueprint 图重写仍然是非目标。

### 11.5 Python 层的保留价值

即使 C++ Plugin 是核心实现，Python 层仍有不可替代的价值：
- YAML Spec 解析（UE5 C++ 无 YAML 库）
- Schema 校验（jsonschema 库）
- CLI 工具（agent-ue5 命令行）
- 快速原型和脚本开发
- MCP Server 集成

---

## 12. 与其他文档的关系

| 文档 | 职责 |
|---|---|
| **本文档** | Bridge 封装层实现方案（三通道 + C++ Plugin 核心） |
| `architecture_overview.md` | 系统架构全图 |
| `ue5_capability_map.md` | 10 个 UE5 官方模块与本方案的映射 |
| `tool_contract_v0_1.md` | 工具的参数、响应、UE5 依赖契约（L1/L2/L3） |
| `mvp_smoke_test_plan.md` | 测试层方案（Automation Test / Spec / Functional Testing） |
| `orchestrator_design.md` | Orchestrator 编排 + Gauntlet 集成 |

