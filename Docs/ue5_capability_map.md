# UE5 官方能力分层映射

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 文档目的

本文档明确 **AGENT + UE5 可操作层** 在整体系统中的定位，以及该层为了实现"可查询、可执行、可验证、可回滚"的目标，需要依赖哪些 **UE5 官方能力模块**。

需要先说明的是：

**AGENT + UE5 可操作层并不是 Unreal Engine 内置的单一官方模块。**
它本质上是位于 **AI Agent 与 Unreal Engine 官方能力之间的一层受控编排层**，负责将分散的 UE5 官方能力统一收编为结构化工具，并对这些工具增加参数约束、权限控制、执行护栏、验证闭环与审计能力。

因此，本层的核心职责不是替代 UE5 官方模块，而是：

- 统一接入 UE5 官方能力
- 将这些能力封装为可调用工具
- 为 Agent 提供稳定、安全、可验证的操作接口
- 将"自然语言需求"转换为"受控的 UE 操作与验证流程"

---

## 2. AGENT + UE5 可操作层在系统中的位置

从整体系统视角看，AGENT + UE5 可操作层位于：

- **上接 Agent 层**（Codex / Claude / OpenCode 等 AI Agent）
- **下接 UE5 官方执行能力**（Python Editor Scripting / Remote Control API / Commandlet / UAT）
- **后接 Verification / Rollback 验证与回滚层**（Automation Test Framework / Functional Testing / Gauntlet / Transaction System）
- **侧向连接代码工具链与版本控制系统**（UBT / Git / Perforce）

其在系统中的核心位置，不是单纯的"AI 层"或"UE 层"，而是：

**以 Tool / Orchestrator 为核心的受控中间层。**

该层承担的核心职责包括：

- 将 UE 能力封装为工具合同（Tool Contracts）
- 做参数校验与调用约束
- 执行查询 → 计划 → dry-run → apply → verify 的闭环
- 管理权限、日志、审计与回滚钩子
- 统一结果格式，供 Agent 继续分析与修正

---

## 3. 系统整体模块图

```text
┌───────────────────────────────────────┐
│              Agent 层                  │
│   Codex / Claude / OpenCode           │
│   - 理解需求                           │
│   - 拆分任务                           │
│   - 选择工具                           │
│   - 读结果后修正                       │
└──────────────────┬────────────────────┘
                   │
                   v
┌───────────────────────────────────────────────────────────┐
│      AGENT + UE5 可操作层（核心位置）                       │
│      Orchestrator / Tool Router / Guardrails               │
│   - 工具契约（Tool Contracts）                              │
│   - 参数校验（禁止模糊字段进入执行层）                       │
│   - 查询 → 计划 → dry-run → apply → verify                │
│   - 权限控制 / 审计日志 / 回滚钩子                          │
│   - 统一响应外壳 {status, summary, data, warnings, errors}  │
│   - Schema 契约层（JSON Schema 数据格式约束）               │
└──────────┬────────────────────────────┬───────────────────┘
           │                            │
           │                            │
           v                            v
┌────────────────────────┐   ┌─────────────────────────────────┐
│   Code / Build Tools   │   │       UE5 Bridge 层              │
│   - UBT / 编译         │   │                                   │
│   - lint / unit tests  │   │  通道 A: Python Editor Scripting  │
│   - Git / Perforce     │   │    unreal 模块（进程内调用）       │
│                        │   │    Editor Scripting Utilities      │
│                        │   │                                   │
│                        │   │  通道 B: Remote Control API        │
│                        │   │    HTTP REST / WebSocket（远程）   │
│                        │   │    generateTransaction 支持        │
│                        │   │                                   │
│                        │   │  通道 C: Commandlet / CLI          │
│                        │   │    无 GUI 批处理                   │
│                        │   │                                   │
│                        │   │  通道 D: UAT                       │
│                        │   │    构建 / 打包 / CI 编排           │
│                        │   │                                   │
│                        │   │  通道 E: 自定义 UE Editor Plugin   │
│                        │   │    C++ 最高可控性（推荐中枢）      │
└───────────┬────────────┘   └───────────────┬─────────────────┘
            │                                │
            │                                v
            │                ┌─────────────────────────────────┐
            │                │       Unreal Engine              │
            │                │   Editor / Assets / Maps         │
            │                │   Blueprint / C++ / UI           │
            │                │   Transaction System (Undo/Redo) │
            │                └───────────────┬─────────────────┘
            │                                │
            └──────────────┬─────────────────┘
                           v
┌───────────────────────────────────────────────────────────┐
│          Verification / Rollback 层                        │
│                                                            │
│   UE5 原生测试体系：                                        │
│   - Automation Test Framework（测试基座）                    │
│   - Automation Spec（BDD 行为式测试）                       │
│   - Functional Testing（关卡内功能测试）                     │
│   - Automation Driver（输入模拟，封装在高层测试中）           │
│   - Gauntlet（会话级 / 端到端 / CI 测试编排）               │
│                                                            │
│   观测与审计：                                              │
│   - Map Check / Editor Logs / Screenshots                  │
│   - Screenshot Comparison Tool                             │
│   - Dirty Asset Report                                     │
│   - Packaging Smoke Test                                   │
│                                                            │
│   回滚：                                                    │
│   - UE5 Transaction System（原生 Undo/Redo）               │
│   - Git / Perforce 版本回退                                 │
└───────────────────────────────────────────────────────────┘
```

---

## 4. AGENT + UE5 可操作层需要接入的 UE5 官方能力模块

### 4.1 编辑器自动化模块

这类模块负责让 Unreal Editor 可以被脚本、外部服务和自动化流程驱动，是可操作层最基础的执行能力来源。

#### 4.1.1 Python Editor Scripting

**UE5 官方定位**：Editor 环境内置 Python 运行时，通过 `unreal` 模块将引擎 C++ API 暴露给 Python。由 Python Editor Script Plugin + Editor Scripting Utilities Plugin 两个插件共同提供。UE5 中默认启用。

**能力范围**：
- 操作关卡中的 Actor（查询 / 生成 / 销毁 / 修改 Transform 和属性）
- 操作资产（导入 / 创建 / 复制 / 修改属性）
- 操作 Blueprint（创建子类 / 修改默认值）
- 读取和修改组件属性（碰撞 / 材质 / Mesh 引用）
- 执行 Editor Console Command
- 运行 Automation Test

**能力边界**：
- **仅限 Editor 环境**——PIE / Standalone / 打包后均不可用
- **不能操作 Blueprint 图的节点和连线**（K2Node）——可创建 Blueprint、修改默认值，但不能添加/删除/连接图中节点
- **不适合 PIE 模式下的写操作**

**在本方案中的角色**：Bridge 层通道 A——进程内直接调用，适合开发调试和快速验证。快速原型的首选执行通道。

**关键 API 子系统**：

| 子系统 | 对应 C++ 类 | 本方案中的用途 |
|---|---|---|
| `unreal.EditorLevelLibrary` | `UEditorLevelLibrary` | list_level_actors / spawn_actor |
| `unreal.EditorAssetLibrary` | `UEditorAssetLibrary` | get_asset_metadata / does_asset_exist |
| `unreal.AssetToolsHelpers` | `UAssetToolsHelpers` | import_assets / create_blueprint_child |
| `unreal.Actor` 基类 | `AActor` | get_actor_state（location/rotation/scale/bounds/tags） |
| `unreal.PrimitiveComponent` | `UPrimitiveComponent` | 碰撞配置读写 |
| `unreal.SystemLibrary` | `UKismetSystemLibrary` | engine_version / console_command |
| `unreal.Paths` | `FPaths` | project_path |

---

#### 4.1.2 Remote Control API

**UE5 官方定位**：Editor 内嵌 Web Server，通过 HTTP REST 和 WebSocket 协议允许外部应用程序远程读写 UE5 Editor 中的对象属性、调用 BlueprintCallable 函数。由 Remote Control API Plugin 提供。

**能力范围**：
- 远程调用任意 UObject 上的 BlueprintCallable 函数（`/remote/object/call`）
- 远程读写 UObject 的公开属性（`/remote/object/property`）
- 批量操作（`/remote/batch`）
- 搜索 Actor 和 Asset（`/remote/search/actors`、`/remote/search/assets`）
- WebSocket 实时订阅属性变更
- **`generateTransaction: true` 将操作纳入 UE5 的 Undo/Redo 事务系统**

**能力边界**：
- 不能调用 private/protected 函数或属性
- 不能直接操作 Blueprint 图节点（同 Python 的限制——底层 API 一致）
- 默认在 Packaged Build 和 -game 模式下禁用
- 需要知道 UObject 的完整路径（objectPath）

**与 Python Editor Scripting 的关键区别**：

| 维度 | Python Editor Scripting | Remote Control API |
|---|---|---|
| 运行位置 | Editor 进程**内部** | Editor 进程**外部**（HTTP） |
| 底层 API | 同一套反射系统 | 同一套反射系统 |
| 适用场景 | 脚本在 Editor 内执行 | Agent / 外部服务远程调用 |
| 事务支持 | 无原生事务 | `generateTransaction: true` 支持 Undo |

**在本方案中的角色**：Bridge 层通道 B——HTTP 远程调用，适合 Agent 从外部进程控制 Editor。是解决"Agent→Editor 通信"的关键通道。已有 MCP Server 实现（`unreal-remote-control-mcp`）。

---

#### 4.1.3 自定义 UE Editor Plugin

**UE5 官方定位**：UE5 的 Editor Module 扩展机制，允许开发者用 C++ 编写 Editor 插件，获得最高级别的引擎访问权限。

**在本方案中的角色**：**v0.3 已实装为 AgentBridge C++ Editor Plugin**——Bridge 封装层的核心实现。包含两个 Plugin：

- **AgentBridge Plugin**（功能层）：`UAgentBridgeSubsystem`（UEditorSubsystem）实现全部 Bridge 接口（查询 7 + 写入 4 + 验证 3 + 构建 1），加上 `UAgentBridgeCommandlet` 和 `FUATRunner`
- **AgentBridgeTests Plugin**（测试层）：注册全部 Automation Test（L1/L2/L3）+ GauntletTestController

C++ Plugin 带来的收益：
- 最高性能和稳定性（通道 C：进程内 C++ 直接调用）
- 原生集成 UE5 Transaction System（FScopedTransaction）
- 注册 Automation Test（引擎原生测试体系）
- 注册 Commandlet（无头执行）
- 通过 Remote Control API 自动暴露 BlueprintCallable 接口给外部调用者

---

### 4.2 批处理与构建模块

#### 4.2.1 Command-Line / Commandlet

**UE5 官方定位**：无头（headless）批处理命令框架。开发者编写继承自 `UCommandlet` 的 C++ 类，通过命令行（`UE5Editor-Cmd.exe -run=<Name>`）在不启动完整 Editor GUI 的情况下执行引擎级操作。

**能力范围**：
- 无 GUI 批量资产操作（ResavePackages / CompileAllBlueprints）
- 无 GUI 本地化流程
- 无 GUI 数据验证
- 通过 `-ExecCmds` 执行 Console Command（包括 `Automation RunTests`）
- CI/CD 流水线中的自动化环节

**在本方案中的角色**：**v0.3 已实装为 `UAgentBridgeCommandlet`**：
- 无头执行 Spec 文件（`-run=AgentBridge -Spec="xxx.yaml"`）
- 无头运行 Automation Test（`-run=AgentBridge -RunTests="Project.AgentBridge"`）
- `MAP CHECK` 的无 GUI 执行路径
- CI/CD 流水线中的标准入口——Gauntlet 通过 Commandlet 进入 Editor 执行测试

**与 UAT 的层次关系**：Commandlet 运行在引擎进程**内部**（C++），UAT 运行在引擎进程**外部**（C#）。UAT 调用 Commandlet，不是并列关系。

---

#### 4.2.2 UAT（Unreal Automation Tool）

**UE5 官方定位**：外部宿主程序（C#），用于编排与 Unreal Engine 相关的无人值守自动化流程。通过 `RunUAT.bat` 启动。

**能力范围**：
- `BuildCookRun`：编译 + 烹饪 + 打包 + 归档
- `BuildTarget` / `CookTarget`：单独编译或烹饪
- `BuildCookRun -run -editortest -RunAutomationTest=<Filter>`：构建后自动运行测试（当前 UE5.5.4 项目稳定路径）
- `GenerateAutomationProject`：生成 Gauntlet 自动化项目
- 自定义 UAT Command（C#）

**在本方案中的角色**：**v0.3 已实装为 `FUATRunner`（C++ 封装）+ `uat_runner.py`（Python 封装）**：
- `build_project` 工具的实现后端（调用 `BuildCookRun`）
- `RunAutomationTests` 集成——当前通过 `BuildCookRun -run -editortest -RunAutomationTest=...` 在 CI/CD 中运行 Automation Test
- Gauntlet 的宿主运行环境（`RunUAT RunGauntlet ...`）
- CI/CD 无人值守流水线的编排入口
- Gauntlet 测试的宿主程序

---

### 4.3 自动化测试模块

这组模块是实现"可验证闭环"的核心基础，负责让系统不仅能改动 UE，还能验证改动是否正确。

#### 4.3.1 Automation Test Framework

**UE5 官方定位**：引擎核心的自动化测试框架（`FAutomationTestBase`），是整个 UE5 测试体系的基座。支持 Simple Test（单元测试）、Complex Test（参数化测试）、Latent Command（跨帧异步操作）。

**Test Flag 分类**：`SmokeFilter` / `ProductFilter` / `EngineFilter` / `PerfFilter` / `StressFilter` + `EditorContext` / `ClientContext` / `ServerContext`

**运行方式**：Session Frontend UI / Console Command `Automation RunTests` / Commandlet `-ExecCmds` / UAT `BuildCookRun -run -editortest -RunAutomationTest`

**在本方案中的角色**：**v0.3 已实装于 AgentBridgeTests Plugin**：
- L1 测试（11 个）注册为 Simple Automation Test（`IMPLEMENT_SIMPLE_AUTOMATION_TEST` 宏）：Query 7 + Write 4
- L3.UITool 测试（4 个）注册为 Simple Automation Test（`IMPLEMENT_SIMPLE_AUTOMATION_TEST` 宏）
- L2 闭环验证（5 个）注册为 Automation Spec：ClosedLoop 3 + UITool 2
- `run_automation_tests` 工具触发的目标框架
- 容差验证使用 `TestEqual` / `TestNearlyEqual` 替代 Python 自建 verifier.py

**v0.2 → v0.3 的核心转变**：

| 维度 | v0.2（Python 自建） | v0.3（UE5 Automation Test） |
|---|---|---|
| 语言 | Python（脚本手工调用） | C++（宏自动注册到引擎） |
| 运行位置 | Editor 外部或内部 Python | Editor 内部（引擎原生） |
| 执行方式 | `python xxx.py` | Session Frontend / `RunTests` / Commandlet / UAT / Gauntlet |
| 结果查看 | 控制台文本输出 | Session Frontend UI（绿/红状态灯） |
| CI/CD 集成 | 无 | Gauntlet 原生支持 |
| 测试过滤 | 无 | Test Flag（Smoke / Product / Editor 等） |
| 断言 | Python assert | `TestEqual` / `TestTrue` / `TestNearlyEqual` |

---

#### 4.3.2 Automation Spec

**UE5 官方定位**：建立在 Automation Test Framework 之上的 BDD（Behavior-Driven Development）风格测试语法。用 `Describe()` / `BeforeEach()` / `It()` / `AfterEach()` 组织测试。文件约定 `<FeatureName>.spec.cpp`。

**在本方案中的角色**：**v0.3 已实装于 AgentBridgeTests Plugin**：
- L2 闭环验证（3 个 .spec.cpp 文件）使用 BDD Spec 语法
- `L2_SpawnReadbackLoop.spec.cpp`：spawn → readback → verify
- `L2_TransformModifyLoop.spec.cpp`：modify → readback → verify
- `L2_ImportMetadataLoop.spec.cpp`：import → metadata check

**术语区分**：本方案中"Spec"指结构化设计文档（scene_spec_template.yaml），UE5 中"Spec"指 BDD 测试语法（.spec.cpp）。两者含义不同。文档中使用"结构化 Spec"指代前者，"Automation Spec"指代后者。

---

#### 4.3.3 Functional Testing

**UE5 官方定位**：关卡内（in-level）测试框架。通过在关卡中放置 `AFunctionalTest` Actor 来执行测试。测试本身在 World 中运行，有位置、有 Tick、可观察和操作同一 World 中的其他 Actor。

**约定**：以 `FTEST_` 为前缀的地图是功能测试地图，Session Frontend 的"Run Level Test"按钮自动识别。

**在本方案中的角色**：**v0.3 已实装于 AgentBridgeTests Plugin**：
- L3 完整 Demo 测试为 `AAgentBridgeFunctionalTest`（AFunctionalTest 子类）
- 放置在 `FTEST_WarehouseDemo` 测试地图中
- 在测试地图中执行完整 Spec → 验证全部 Actor → 判定 Pass/Fail
- 关卡级验证（inside_bounds / non_overlap）的原生实现路径
- Blueprint 可编写测试——TA 可参与

---

#### 4.3.4 Automation Driver

**UE5 官方定位**：应用层用户输入模拟引擎。模拟鼠标移动 / 点击 / 键盘输入 / 拖拽 / 滚动等桌面端交互行为。设计为与 Automation Spec 配合使用。

**在本方案中的角色**：**L3 UI 工具层的官方执行后端**。

Automation Driver 不是默认主干执行路径（主干是 L1 语义工具）。它是三层受控工具体系中的第三层——仅当 L1 语义工具无法覆盖某个操作时的受约束补充路径。

**v0.3 已实装的 L3 接口**（3 个 + 1 个辅助）：

| L3 接口 | 功能 | Automation Driver 用法 | L1 验证方式 |
|---|---|---|---|
| `ClickDetailPanelButton` | 在 Detail Panel 中点击按钮 | 默认走 `start_ui_operation/query_ui_operation` 异步任务壳；底层保留非 GameThread 的 `AutomationDriver::Click()` 原型 | GetActorState / GetComponentState |
| `TypeInDetailPanelField` | 在属性输入框中输入值 | 默认走异步任务壳；定位属性行后直接对文本控件设值并显式提交 | GetActorState |
| `DragAssetToViewport` | 从 Content Browser 拖拽资产到 Viewport | 默认走异步任务壳；最终放置走 `DropObjectsAtCoordinates(...)` 官方链路 | ListLevelActors + GetActorState |
| `IsAutomationDriverAvailable` | 检查 Driver 是否可用 | IsModuleLoaded("AutomationDriver") | — |

**实装位置**：
- 封装层：`AutomationDriverAdapter.h/.cpp`（将底层坐标级操作封装为语义级操作）
- Subsystem 接口：`UAgentBridgeSubsystem` 的 4 个 `Category="AgentBridge|UITool"` UFUNCTION
- 交叉比对：`FBridgeUIVerification` 结构体 + `CrossVerifyUIOperation()` 方法
- Python 客户端：`Scripts/bridge/ui_tools.py`

**L3→L1 交叉比对**：每次 L3 操作后，L3 返回值与 L1 独立读回值做对比。两者一致 → success，不一致 → mismatch（含字段级差异列表）。这确保了即使 UI 操作的确定性较低，最终结果仍然经过语义级验证。

**RC 同步链路约束**：UE5 官方对 Automation Driver 同步 API 有明确线程约束。当前工程已验证，L3 UI 工具若继续在 RC 同步请求里直接等待完整 UI 交互，存在阻塞风险。因此 v0.3 的实际落地口径是“L3 统一走异步任务壳，再补 L1 读回验证”。

**测试覆盖**：
- L3 UITool 测试（4 个 Simple Automation Test）：参数校验 + dry_run + Driver 不可用 graceful degradation + 交叉比对
- L2 UITool 闭环（2 个 Automation Spec）：DragAssetToViewportLoop（5 It）+ TypeInFieldLoop（3 It）

**使用判定条件**（全部满足才允许）：
1. L1 无对应 API
2. 操作可用结构化参数描述（路径/名称，非屏幕坐标）
3. 结果可通过 L1/L2 工具独立验证
4. 操作可逆或低风险
5. Spec 中显式标注 `execution_method: ui_tool`
6. 已封装为 AgentBridge Subsystem 接口

**本方案拒绝的是"无边界 GUI 自动化"**——Agent 直接发送屏幕坐标级指令、绕过 Spec 和 AgentBridge。**接受的是"经过严格封装和验证的 UI 工具"**——通过 AgentBridge 接口、结构化参数、L1 独立验证的 UI 操作。

---

#### 4.3.5 Gauntlet

**UE5 官方定位**：外部测试会话编排框架，基于 UAT。负责启动一个或多个 Unreal 进程实例，在其中执行测试，收集结果。通过 `UGauntletTestController` 在引擎内部驱动测试。

**在本方案中的角色**：**v0.3 已实装**：
- `Gauntlet/AgentBridge.TestConfig.cs`：C# 测试会话配置（定义角色、超时、Controller）
- `AgentBridgeGauntletController`（UGauntletTestController 子类）：在 Editor 内驱动测试执行
- 通过 `RunUAT RunGauntlet -Test=AgentBridge.TestConfig ...` 启动
- 提供完整的进程管理（启动 / 监控 / 超时 / 崩溃恢复 / 停止）
- 与 Orchestrator 的关系：Gauntlet 编排"在什么环境下运行"，Orchestrator 编排"运行什么内容"

---

### 4.4 验证与观测能力

除了上述"执行模块"与"测试模块"，可操作层还依赖一组验证与观测能力：

| 能力 | UE5 来源 | 在本方案中的用途 |
|---|---|---|
| Map Check | Editor 内置 `MAP CHECK` 命令 | `run_map_check` 工具 |
| Editor Logs | `FOutputDevice` / Log 系统 | `get_editor_log_tail` 工具 |
| Screenshots | Editor 截图 API | `capture_viewport_screenshot` 工具 |
| Screenshot Comparison | UE5 内置 Screenshot Comparison Tool | v0.2 截图自动对比 |
| Dirty Asset Report | Package 脏状态管理 | `get_dirty_assets` 工具 |
| Transaction System | `GEditor->BeginTransaction()` / `EndTransaction()` / Remote Control `generateTransaction` | 基础 Undo 能力 |
| Git / Perforce | 外部 VCS | 代码和资产级回滚 |

这些能力的作用不在于"执行操作"，而在于判断操作是否成功、是否产生副作用、是否可接受、为回滚提供证据。**验证层不是附属能力，而是与执行层同等重要的核心组成部分。**

---

## 5. 分层映射：我们的方案 ↔ UE5 官方能力

### 5.1 Agent 层 → 无直接 UE5 依赖

Agent 层（Spec 生成 / 任务规划 / 工具选择 / 结果分析）是本方案的自主设计。UE5 没有"将自然语言转为结构化操作"的能力。

### 5.2 Orchestrator 层 → Gauntlet 实际集成

| 维度 | 我们的 Orchestrator | UE5 Gauntlet | v0.3 集成方式 |
|---|---|---|---|
| 运行位置 | 引擎外部（Python）或内部（Commandlet） | 引擎外部（C#，基于 UAT） | 两者协作 |
| 职责 | 编排开发操作 | 编排测试会话 | Gauntlet 管环境，Orchestrator 管内容 |
| 引擎内触角 | AgentBridgeSubsystem | GauntletTestController | AgentBridgeGauntletController 同时继承两者 |
| 结果收集 | JSON 返回值 + 报告 | 退出码 + 日志 + Saved 文件 | 同时输出两种格式 |
| 超时机制 | bridge_core.py timeout 参数 | C# MaxDuration | 两级超时 |
| 进程管理 | 通过 RC API 检测 Editor 健康 | **自动启动/监控/停止** | Gauntlet 管理进程 |

### 5.3 通信层 → Remote Control API / Python Remote Execution

**这是当前方案新增的关键选型。** Agent 运行在 Editor 外部，需要通过通信协议调用 Bridge。

| 通道 | 机制 | 适用场景 | 推荐 |
|---|---|---|---|
| Remote Control API | HTTP REST | Agent 远程控制 Editor；支持 Transaction | ✅ 推荐 |
| Python Remote Execution | Python Plugin 内置 UDP+TCP | Python→Python 通信 | 备选 |
| 自建协议 | 文件/Socket | — | ❌ 不推荐 |

推荐路径：**C++ Plugin 为核心底层（通道 C），Python 脚本用通道 A 快速原型，Agent 远程调用用通道 B（Remote Control API）。** Bridge 封装层设计为三通道——底层可切换，上层接口不变。

### 5.4 Bridge 层 → C++ AgentBridge Plugin（核心） + Python / RC API（客户端）

Bridge 层的全部能力来自 UE5 官方 API。v0.3 中核心实现为 C++ Editor Plugin（`UAgentBridgeSubsystem`），Python 和 RC API 作为客户端调用 Plugin 暴露的接口。

Bridge 封装层增加的价值（UE5 原生 API 不提供的）：
- 统一响应外壳 `{status, summary, data, warnings, errors}`
- 参数预校验（C++ `ValidateTransform()` / Python `validate_transform()`）
- 统一错误码（ACTOR_NOT_FOUND / INVALID_ARGS 等）
- Schema 对齐（返回值符合 JSON Schema 定义）
- 写后读回（actual_transform 自动读回）
- Transaction 管理（C++ `FScopedTransaction` 原生 Undo）
- Mock 模式（开发调试用）

### 5.5 Verification 层 → UE5 Automation Test Framework（已实装）

| 验证能力 | v0.3 实现 | UE5 官方能力 | 实装位置 |
|---|---|---|---|
| 容差比对 | C++ `TestNearlyEqual` | Automation Test 断言 | AgentBridgeTests Plugin |
| L1 单接口测试 | `IMPLEMENT_SIMPLE_AUTOMATION_TEST`（11 个） | Simple Automation Test + ProductFilter | `L1_QueryTests.cpp` / `L1_WriteTests.cpp` |
| L3.UITool 单接口测试 | `IMPLEMENT_SIMPLE_AUTOMATION_TEST`（4 个） | Simple Automation Test + ProductFilter | `L1_UIToolTests.cpp` |
| L2 闭环测试 | `BEGIN_DEFINE_SPEC`（5 个） | Automation Spec + ProductFilter（UE5.5 控制台稳定路径） | `L2_ClosedLoopSpecs.spec.cpp` / `L2_UIToolClosedLoopSpec.spec.cpp` |
| L3 完整 Demo | AFunctionalTest 子类 | Functional Testing + FTEST_ 地图 | `L3_FunctionalTestActor.cpp` |
| L3→L1 交叉比对 | `FBridgeUIVerification` + `CrossVerifyUIOperation` | L3 UI 操作后 L1 独立读回比对 | `BridgeTypes.h` + `AgentBridgeSubsystem.cpp` |
| Schema 校验 | validate_examples.py（Python 保留） | 无 UE5 对应 | **保留**（编排层自有能力） |

### 5.6 Rollback 层 → UE5 Transaction System（C++ 原生集成）

UE5 的 Transaction System 提供原生 Undo/Redo 能力。v0.3 中通过 C++ Plugin 深度集成：

| 方式 | 实现 | 适用场景 |
|---|---|---|
| C++ FScopedTransaction | AgentBridgeSubsystem 的写接口自动使用 | 通道 C（最完整） |
| Remote Control `generateTransaction: true` | 通道 B HTTP 调用 | Agent 远程调用 |
| Python `unreal.EditorTransaction` | 通道 A | Python 脚本 |

C++ `FScopedTransaction` 是最标准的方式——作用域结束时自动提交事务，异常时自动回滚。

---

## 6. 工具 → UE5 API 映射总表

| 工具 | UE5 子系统 | 具体 API | 通道 |
|---|---|---|---|
| `get_current_project_state` | Paths + SystemLibrary + EditorLevelLibrary | `FPaths` / `GetEngineVersion()` / `GetEditorWorld()` | A / B |
| `list_level_actors` | EditorLevelLibrary | `GetAllLevelActors()` | A / B |
| `get_actor_state` | Actor 基类 + PrimitiveComponent | `GetActorLocation()` / `GetActorRotation()` / `GetActorScale3D()` / 碰撞 API | A / B |
| `get_actor_bounds` | Actor 基类 | `GetActorBounds()` | A / B |
| `get_asset_metadata` | EditorAssetLibrary | `DoesAssetExist()` / `FindAssetData()` | A / B |
| `get_dirty_assets` | EditorLoadingAndSavingUtils | `GetDirtyContentPackages()` | A / B |
| `run_map_check` | Editor Console Command | `MAP CHECK` | A / C |
| `get_editor_log_tail` | Log 系统 | `FOutputDevice` | A |
| `spawn_actor` | EditorLevelLibrary | `SpawnActorFromClass()` | A / B |
| `set_actor_transform` | Actor 基类 | `SetActorLocationAndRotation()` + `SetActorScale3D()` | A / B |
| `import_assets` | AssetTools | `ImportAssetTasks()` | A |
| `create_blueprint_child` | BlueprintFactory + AssetTools | `CreateAsset()` | A |
| `build_project` | **UAT**（外部程序） | `BuildCookRun` | D |
| `run_automation_tests` | **Automation Test Framework** | `Automation RunTests <Filter>` | C / D |
| `save_named_assets` | EditorAssetLibrary | `SaveLoadedAssets()` | A / B |

通道说明：A = Python Editor Scripting / B = Remote Control API / C = Commandlet / D = UAT

---

## 7. 推荐落地优先级

### 7.1 第一优先级：打通执行通道（第 1-3 周）

优先接入：
- Python Editor Scripting（通道 A——进程内快速验证）
- Remote Control API（通道 B——Agent 远程调用）
- Commandlet（通道 C——`run_automation_tests` 的 CI 入口）

目标：查询项目状态 → 执行低风险编辑器操作 → 建立统一工具入口 → Agent 可远程控制 Editor

### 7.2 第二优先级：接通验证闭环（第 4-6 周）

优先接入：
- Automation Test Framework（`run_automation_tests` 对接）
- Map Check / Editor Logs / Dirty Asset Report（观测能力）
- UE5 Transaction System（基础 Undo）

目标：操作后可验证 → 验证失败可定位 → 基础回滚可用

### 7.3 第三优先级：增强运行时与回归能力（v0.2）

后续接入：
- Functional Testing（关卡内验证）
- Automation Spec（BDD 测试迁移）
- Gauntlet（CI/CD 编排）
- UAT BuildCookRun（构建流水线）
- Screenshot Comparison（截图自动对比）

目标：核心测试迁移到 UE5 原生体系 → CI/CD 无人值守 → 工程化规模运行

---

## 8. 本章结论

**AGENT + UE5 可操作层** 在系统中的定位明确为：

**位于 Agent 之下、位于 UE5 官方能力之上、负责把分散的 UE5 官方模块统一收编为可调用平台的一层受控中间层。**

v0.3 中，它以 **C++ Editor Plugin** 为核心实现，全部 10 个 UE5 官方模块已在代码中实装：

| 模块 | 实装方式 | 实装状态 |
|---|---|---|
| Python Editor Scripting | 通道 A 客户端 | ✅ 已实装 |
| Remote Control API | 通道 B 客户端 + Plugin RC 暴露 | ✅ 已实装 |
| 自定义 C++ Plugin | AgentBridge Plugin + AgentBridgeTests Plugin | ✅ 已实装 |
| Command-Line / Commandlet | UAgentBridgeCommandlet | ✅ 已实装 |
| UAT | FUATRunner + uat_runner.py | ✅ 已实装 |
| Automation Test Framework | L1 Simple Automation Test（11 个：Query 7 + Write 4）+ L3.UITool（4 个） | ✅ 已实装 |
| Automation Spec | L2 闭环验证 .spec.cpp（5 个：ClosedLoop 3 + UITool 2） | ✅ 已实装 |
| Functional Testing | L3 AFunctionalTest 子类 + FTEST_ 地图 | ✅ 已实装 |
| Automation Driver | **L3 UI 工具执行后端**（3 接口 + 封装层 + L3→L1 交叉比对） | ✅ 已实装 |
| Gauntlet | C# TestConfig + GauntletTestController | ✅ 已实装 |

在此基础上，再叠加编排层自有能力（UE5 不提供的）：

- 三层受控工具体系（L1 语义工具 > L2 编辑器服务工具 > L3 UI 工具）
- 工具契约（Tool Contracts）
- 参数校验与禁止模糊字段
- 权限控制
- 审计与日志
- 验证闭环（写后读回 + 容差比对 + L3→L1 交叉比对 + 结构化报告）
- 回滚能力（基于 UE5 Transaction System）
- Schema 数据格式契约（JSON Schema）
- 统一响应外壳与错误码体系
- 结构化 Spec 驱动编排（YAML Spec，含 `execution_method` 标注）

最终形成一个**以 UE5 官方能力为底座、真正可被 Agent 稳定调用的 UE5 可操作平台**。

