# 系统架构概述

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

---

## 1. 核心定位

**AGENT + UE5 可操作层**不是 UE5 内置模块，而是**位于 AI Agent 与 UE5 官方能力之间的受控编排层**。

它的全部执行能力来自 UE5 官方 API。我们不创造新的引擎 API——每个操作接口都是对 UE5 官方能力的结构化封装，增加参数约束、权限控制、执行护栏、验证闭环与审计能力。

### 1.1 v0.3 核心变更

v0.3 的核心变更是**将 Bridge 封装层从 Python 脚本升级为 C++ Editor Plugin**，同时将全部 10 个 UE5 官方模块从"文档提及"变为"代码实装"：

| 维度 | v0.2 | v0.3 |
|---|---|---|
| Bridge 核心 | Python 脚本（调用 `unreal` 模块 / HTTP） | **C++ Editor Plugin**（UE5 原生模块） |
| 工具分层 | 无分层（统称"受控工具"） | **三层体系**：L1 语义工具 > L2 编辑器服务工具 > L3 UI 工具 |
| 验证层 | Python 自建（脚本手工调用） | **UE5 Automation Test Framework**（引擎原生） |
| UI 自动化 | 完全排除 | **L3 UI 工具**：Automation Driver 作为受约束执行后端纳入 |
| 无头执行 | 无 | **Commandlet**（引擎原生批处理） |
| 构建自动化 | 无 | **UAT**（引擎原生构建工具） |
| CI/CD 编排 | 无 | **Gauntlet**（引擎原生测试会话框架） |
| Python 定位 | Bridge 核心实现 | 轻量客户端（通过 RC API 调用 C++ Plugin） |

---

## 2. 系统架构全图

```
                    ┌──────────────────────────────────────────┐
                    │              AI Agent                     │
                    │      （Claude / GPT / 自研 Agent）         │
                    └──────────────┬───────────────────────────┘
                                   │
                    ┌──────────────▼───────────────────────────┐
                    │         Orchestrator 编排层               │
                    │  Spec 解析 → 计划生成 → 执行 → 验证 → 报告  │
                    │  Python CLI / C# Gauntlet Controller     │
                    └──────────────┬───────────────────────────┘
                                   │
               ┌───────────────────┼───────────────────┐
               │                   │                   │
    ┌──────────▼─────┐  ┌─────────▼────────┐  ┌──────▼──────────┐
    │ 通道 A: Python  │  │ 通道 B: RC API   │  │ 通道 C: C++ 直接 │
    │ import unreal   │  │ HTTP PUT :30010  │  │ 进程内调用       │
    │ 进程内脚本      │  │ Agent 远程       │  │ 最高性能         │
    └────────┬────────┘  └────────┬─────────┘  └───────┬─────────┘
             │                    │                     │
    ┌────────▼────────────────────▼─────────────────────▼─────────┐
    │                                                              │
    │              AgentBridge C++ Editor Plugin                   │
    │              ════════════════════════════                    │
    │                                                              │
    │  ┌─────────────────────┐  ┌─────────────────────────────┐   │
    │  │  UAgentBridgeSubsystem │  │  UAgentBridgeCommandlet    │   │
    │  │  (UEditorSubsystem)    │  │  (UCommandlet)             │   │
    │  │                        │  │                             │   │
    │  │  L1 Query 接口 (7个)   │  │  无头 Spec 执行            │   │
    │  │  L1 Write 接口 (4个)   │  │  无头测试执行              │   │
    │  │  L2 Validate/Build (4) │  │  CI/CD 入口               │   │
    │  │  L3 UITool 接口 (3个)  │  │                             │   │
    │  │  参数校验 + 统一响应   │  │                             │   │
    │  │  Transaction 管理      │  │                             │   │
    │  │  L3→L1 交叉比对       │  │                             │   │
    │  └───────────┬────────────┘  └──────────┬──────────────────┘   │
    │              │                           │                     │
    │  ┌───────────▼───────────────────────────▼──────────────────┐ │
    │  │                 UE5 官方 API 层                           │ │
    │  │  L1: EditorLevelLibrary / EditorAssetLibrary / AssetTools │ │
    │  │  L1: Remote Control API / FScopedTransaction             │ │
    │  │  L2: Automation Test / Spec / Functional Testing         │ │
    │  │  L3: Automation Driver（IAutomationDriverModule）        │ │
    │  └──────────────────────────────────────────────────────────┘ │
    │                                                              │
    ├──────────────────────────────────────────────────────────────┤
    │                                                              │
    │              AgentBridgeTests C++ Plugin                     │
    │              ═══════════════════════════                     │
    │                                                              │
    │  L1: Simple Automation Test（11 个：Query 7 + Write 4）      │
    │  L3.UITool: Simple Automation Test（4 个）                  │
    │  L2: Automation Spec（5 个：ClosedLoop 3 + UITool 2）       │
    │  L3: Functional Testing（AFunctionalTest 子类）             │
    │                                                              │
    └──────────────────────────────────────────────────────────────┘
                                   │
    ┌──────────────────────────────▼───────────────────────────────┐
    │              UE5.5.4 Editor / Commandlet / UAT              │
    └──────────────────────────────────────────────────────────────┘
                                   │
    ┌──────────────────────────────▼───────────────────────────────┐
    │              Gauntlet（C# 测试会话编排）                     │
    │  启动 Editor → 运行测试 → 收集结果 → 判定通过/失败          │
    └─────────────────────────────────────────────────────────────┘
```

---

## 3. 10 个 UE5 官方模块实装状态

v0.3 要求全部 10 个模块在代码中实装（不是"文档提及"）。按三层工具体系分类：

| 模块 | 所属层次 | 实装方式 | 实装位置 | 验证方式 |
|---|---|---|---|---|
| **Python Editor Scripting** | L1 语义 | 通道 A：Python 脚本进程内调用 `unreal` 模块 | `Scripts/bridge/` | L1 测试通道 A 分支 |
| **Remote Control API** | L1 语义 | 通道 B：HTTP 客户端调用 RC 端点 | `remote_control_client.py` + Plugin RC 暴露 | L1 测试通道 B 分支 |
| **C++ Editor Plugin** | L1 语义 | 通道 C：UAgentBridgeSubsystem（核心） | `Plugins/AgentBridge/` | 全部 L1/L2 测试 |
| **Commandlet** | L2 服务 | `UAgentBridgeCommandlet`：无头执行 Spec / 测试 | `AgentBridgeCommandlet.cpp` | 命令行 `-run=AgentBridge` |
| **UAT** | L2 服务 | `FUATRunner`：封装 BuildCookRun / RunTests | `UATRunner.cpp` | `build_project` 可调用 |
| **Automation Test Framework** | L2 服务 | L1 测试注册为 Simple Automation Test（11 个）+ L3.UITool（4 个） | `L1_*.cpp` | Session Frontend |
| **Automation Spec** | L2 服务 | L2 测试用 BDD Spec 语法（5 个） | `L2_*.spec.cpp` | Session Frontend |
| **Functional Testing** | L2 服务 | L3 测试为 AFunctionalTest 子类 | `L3_*.cpp` | FTEST_ 地图 |
| **Gauntlet** | L2 服务 | C# 测试会话配置 + GauntletTestController | `Gauntlet/*.cs` + `GauntletController` | UAT RunGauntlet |
| **Automation Driver** | **L3 UI** | **L3 UI 工具的官方执行后端**（3 个接口 + 封装层 + 交叉比对） | `AutomationDriverAdapter.cpp` + Subsystem UITool 接口 | L3 UITool 测试 + L2 UITool 闭环 |

---

## 3.5 三层受控工具体系

本方案的核心执行范式是：

> 自然语言/需求 → 结构化 Spec → **受控工具调用** → 写后读回 → 独立反馈验证 → 可判定结果

"受控工具调用"按确定性和风险等级分为三层：

| 层次 | 名称 | 优先级 | 执行后端 | 默认主干？ |
|---|---|---|---|---|
| **L1** | **语义工具** | 最高 | UE5 C++ API / BlueprintCallable | ✅ 是 |
| **L2** | **编辑器服务工具** | 中 | Commandlet / UAT / Console Command | — 服务型 |
| **L3** | **UI 工具** | 最低 | Automation Driver / 模拟输入 | ❌ 仅 fallback |

**优先级规则**：Agent 在选择工具时，**必须**按 L1 > L2 > L3 优先级。仅当上一层没有对应能力时，才降级到下一层。

### L1 语义工具（Semantic Tools）

通过 UE5 C++ API 直接操作引擎对象。参数是结构化数据（路径/坐标/类名），执行确定性最高，可读回、可回滚、可验证。不依赖 UI 状态。

典型接口：`SpawnActor` / `SetActorTransform` / `GetActorState` / `GetActorBounds` / `ImportAssets` / `CreateBlueprintChild` / `ValidateActorInsideBounds`

UE5 模块：Python Editor Scripting + Remote Control API + C++ Editor Plugin

### L2 编辑器服务工具（Editor Service Tools）

调用 UE5 编辑器的工程级服务能力。不直接操作场景内容，提供构建、测试、验证、日志等基础设施。

典型接口：`BuildProject` / `RunAutomationTests` / `RunMapCheck` / `GetDirtyAssets` / `SaveNamedAssets` / `CaptureViewportScreenshot`

UE5 模块：Commandlet + UAT + Automation Test Framework + Automation Spec + Functional Testing + Gauntlet

### L3 UI 工具（UI Tools）

通过 Automation Driver 与 Slate/Editor 原生放置链组合执行 UI 级操作。仅用于 L1 无法覆盖的 UI 级操作场景。

典型接口：`ClickDetailPanelButton` / `TypeInDetailPanelField` / `DragAssetToViewport`

UE5 模块：Automation Driver（`IAutomationDriverModule`）+ Slate + LevelEditor

**统一执行模型**：L3 默认不再把完整 UI 操作直接塞进 RC 同步调用链里等待完成，而是通过 `start_ui_operation()` / `query_ui_operation()` 的异步任务壳调度。原因是 UE5 官方对 Automation Driver 同步 API 有明确线程约束；在 RC 同步链路里直接等待完整 UI 操作，存在阻塞和死锁风险。

**当前已落地的后端口径**：
- `ClickDetailPanelButton`：默认包装函数走异步任务壳，底层使用非 GameThread 上的 `AutomationDriver::Click()` 原型验证点击语义
- `TypeInDetailPanelField`：走异步任务壳；属性行定位后对 `SEditableText` 直接设值，并通过 `Enter` 提交，最后再做 L1 读回
- `DragAssetToViewport`：走异步任务壳；最终放置通过 Editor 官方 `DropObjectsAtCoordinates(...)` 路径完成，而不是裸鼠标拖拽

**L3 使用约束**（全部满足才允许）：
1. L1 无对应 API
2. 操作可用结构化参数描述（路径/名称，非屏幕坐标）
3. 结果可通过 L1/L2 工具独立验证
4. 操作可逆或低风险
5. Spec 中显式标注 `execution_method: ui_tool`
6. 已封装为 AgentBridge Subsystem 接口

**L3→L1 交叉比对**：每次 L3 操作后，L3 返回值与 L1 独立读回值做对比。两者一致 → success，不一致 → mismatch（含字段级差异）。交叉比对通过 `FBridgeUIVerification` 结构体实现。

**本方案拒绝的是"无边界 GUI 自动化"**——Agent 直接发送屏幕坐标级指令、绕过 Spec 和 AgentBridge。**接受的是"经过严格封装和验证的 UI 工具"**——通过 AgentBridge 接口、结构化参数、独立验证的 UI 操作。

---

## 4. 两个 C++ Plugin 的职责分工

### 4.1 AgentBridge Plugin（核心功能）

**类型**：Editor Plugin（仅在 Editor 环境加载，不进入打包游戏）

**职责**：
- 提供全部 Bridge 接口的 C++ 原生实现（查询 7 个 + 写入 4 个 + 验证 3 个 + 构建 1 个）
- 管理 UE5 Transaction System（Undo/Redo）
- 提供 Commandlet 无头执行入口
- 封装 UAT 子进程调用
- 通过 Remote Control API 暴露全部接口给外部调用者（Python / Agent / MCP）

**核心类**：

| 类 | 父类 | 职责 |
|---|---|---|
| `UAgentBridgeSubsystem` | `UEditorSubsystem` | 全部 Bridge 接口实现 + 参数校验 + 统一响应 |
| `UAgentBridgeCommandlet` | `UCommandlet` | 无头执行 Spec / 无头运行测试 |
| `FUATRunner` | — | UAT 子进程封装（BuildCookRun / RunAutomationTests） |

**为什么选择 UEditorSubsystem**：
- 随 Editor 启动自动实例化，无需手工创建
- 生命周期与 Editor 一致
- 可通过 `GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>()` 在任何 Editor 代码中获取
- Remote Control API 可直接暴露 Subsystem 的 BlueprintCallable 函数

### 4.2 AgentBridgeTests Plugin（测试专用）

**类型**：Editor Plugin（仅在 Editor 环境 + 测试场景加载）

**职责**：
- 注册全部 Automation Tests（L1/L2/L3）
- 提供 GauntletTestController 子类
- 不包含任何业务逻辑——只调用 AgentBridge Plugin 的接口并验证结果

**依赖关系**：AgentBridgeTests 依赖 AgentBridge（单向）

**为什么分两个 Plugin**：
- AgentBridge 是"功能"，AgentBridgeTests 是"测试"——职责分离
- 用户可以只启用 AgentBridge 而不加载测试代码
- 测试 Plugin 可以在 CI/CD 中按需加载

---

## 5. 通道架构（三通道 + C++ 核心）

### 5.1 通道对比表

| 维度 | 通道 A: Python 进程内 | 通道 B: Remote Control API | 通道 C: C++ Plugin 直接 |
|---|---|---|---|
| 调用方式 | `import unreal` | HTTP PUT `:30010` | `Subsystem->FunctionName()` |
| 运行位置 | Editor 进程内（Python） | Editor 进程外（HTTP） | Editor 进程内（C++） |
| 性能 | 中（Python 解释器开销） | 低（HTTP + JSON 序列化） | **最高**（原生 C++ 调用） |
| Transaction 支持 | 需手动 | `generateTransaction: true` | **原生 FScopedTransaction** |
| 适用场景 | 快速原型、脚本开发 | Agent 远程调用、外部集成 | Plugin 内测试、Commandlet |
| 底层 API | 同一套 UE5 反射系统 | 同一套 UE5 反射系统 | **直接调用 UE5 C++ API** |
| Automation Test 中 | 可用（IPythonScriptPlugin） | 可用（FHttpModule） | **推荐**（最直接） |

### 5.2 调用链路图

```
AI Agent / CLI / MCP
    │
    ├─ 通道 A ─→ Python import unreal → unreal.xxx() → UE5 API
    │
    ├─ 通道 B ─→ HTTP PUT :30010/remote/object/call
    │              → Remote Control API → UE5 反射系统 → UE5 API
    │
    └─ 通道 C ─→ C++ 直接调用
                   → UAgentBridgeSubsystem::SpawnActor()
                   → UEditorLevelLibrary::SpawnActorFromClass()
                   → UE5 API

所有通道最终到达同一套 UE5 底层 API。
C++ Plugin 是三个通道的共同底层——Python 和 RC API 也可以调用 Plugin 暴露的接口。
```

### 5.3 通道选择推荐

| 场景 | 推荐通道 | 原因 |
|---|---|---|
| Automation Test（L1/L2/L3） | 通道 C | 最直接，无序列化开销 |
| Agent 远程调用 | 通道 B | Agent 运行在 Editor 外部 |
| Commandlet 无头执行 | 通道 C | Commandlet 在 Editor 进程内 |
| Python 脚本快速原型 | 通道 A | 最快迭代 |
| MCP Server 集成 | 通道 B | MCP 基于 HTTP |
| CI/CD (Gauntlet) | 通道 C（经 Commandlet）| Gauntlet 启动 Editor 进程，内部用 C++ |

---

## 6. 验证层架构（UE5 Automation Test 驱动）

### 6.1 三层测试体系

```
验证层
│
├── Schema 校验（Python 保留——UE5 无 JSON Schema 对应物）
│   validate_examples.py
│   验证 JSON 响应结构是否符合 Schema 定义
│
├── L1: Simple Automation Test（11 个单接口测试）
│   注册：IMPLEMENT_SIMPLE_AUTOMATION_TEST 宏
│   分组：Query 7 个 + Write 4 个 + UITool 4 个
│   逻辑：调用 Subsystem 接口 → TestEqual / TestTrue 验证返回值
│         UITool 测试含 L3→L1 交叉比对（FBridgeUIVerification）
│   标签：EditorContext + ProductFilter
│   可见：Session Frontend UI → Project.AgentBridge.L1.*
│   执行：UI 手工 / Automation RunTests / Commandlet / UAT / Gauntlet
│
├── L2: Automation Spec（5 个闭环验证）
│   注册：BEGIN_DEFINE_SPEC / DEFINE_SPEC 宏
│   分组：ClosedLoop 3 个（语义工具闭环）+ UITool 2 个（L3→L1 交叉比对闭环）
│   语法：Describe / BeforeEach / It / AfterEach（BDD 风格）
│   逻辑：spawn → readback → verify 闭环 / L3 UI 操作 → L1 读回 → 交叉比对
│   标签：EditorContext + ProductFilter（UE5.5 控制台稳定路径）
│   可见：Session Frontend UI → Project.AgentBridge.L2.*
│
└── L3: Functional Testing（完整 Demo）
    注册：AFunctionalTest 子类放置在 FTEST_ 测试地图中
    逻辑：执行完整 Spec → 验证全部 Actor → 判定 Pass/Fail
    标签：EditorContext + ProductFilter
    可见：Session Frontend UI → Project.AgentBridge.L3.*
    地图：Content/Tests/FTEST_WarehouseDemo.umap
```

### 6.2 与 v0.2 Python 测试的对照

| 维度 | v0.2 Python 测试 | v0.3 UE5 Automation Test |
|---|---|---|
| 注册方式 | Python 脚本手工调用 | C++ 宏自动注册到引擎 |
| 执行方式 | 命令行 `python xxx.py` | Session Frontend / `RunTests` / Commandlet / UAT |
| 结果查看 | 控制台文本输出 | Session Frontend UI（绿/红状态灯） |
| CI/CD 集成 | 无 | Gauntlet 原生支持 |
| 测试过滤 | 无 | Test Flag（Smoke / Product / Editor 等） |
| 断言 | Python assert | `TestEqual` / `TestTrue` / `TestNearlyEqual`（含容差） |
| 超时 | 无 | 引擎内置超时机制 |

### 6.3 容差验证

v0.2 中 `verifier.py` 的自建容差比对逻辑，在 v0.3 中替换为 UE5 Automation Test 的原生断言：

| v0.2 (Python) | v0.3 (C++ Automation Test) |
|---|---|
| `abs(actual - expected) <= 0.01` | `TestNearlyEqual(Actual, Expected, 0.01f)` |
| `assert status == "success"` | `TestEqual(TEXT("Status"), Response.Status, TEXT("success"))` |
| `if actual != expected: report.add_mismatch(...)` | `AddError(FString::Printf(TEXT("Mismatch: %s"), ...))` |

---

## 7. 执行层架构

### 7.1 四种执行模式

| 模式 | 入口 | UE5 官方模块 | 适用场景 |
|---|---|---|---|
| **交互式** | Editor UI / Python 控制台 | Python Editor Scripting | 开发调试 |
| **远程调用** | HTTP PUT / MCP | Remote Control API | Agent 集成 |
| **无头批处理** | `UE5Editor-Cmd -run=AgentBridge` | Commandlet | CI/CD 中执行 Spec |
| **构建自动化** | `RunUAT BuildCookRun` | UAT | 项目构建 |

### 7.2 Commandlet 执行链路

```bash
# 无头执行 Spec
UE5Editor-Cmd.exe MyGame.uproject \
    -run=AgentBridge \
    -Spec="AgentSpecs/levels/warehouse.yaml" \
    -Report="Artifacts/reports/warehouse_report.json" \
    -Unattended -NoPause -NullRHI

# 无头运行自动化测试
UE5Editor-Cmd.exe MyGame.uproject \
    -run=AgentBridge \
    -RunTests="Project.AgentBridge" \
    -Unattended -NoPause -NullRHI
```

### 7.3 UAT 执行链路

```bash
# 构建项目
RunUAT.bat BuildCookRun \
    -project=MyGame.uproject \
    -platform=Win64 -clientconfig=Development \
    -build -cook -stage -pak

# 通过 UAT 运行自动化测试（UE5.5 推荐）
RunUAT.bat BuildCookRun \
    -project=MyGame.uproject \
    -run -editortest \
    -RunAutomationTest=Project.AgentBridge.L2 \
    -unattended -nullrhi -NoP4
```

### 7.4 旧入口命令护栏（CI）

```bash
# 返回 0：未发现旧入口命令
# 返回 1：发现旧入口命令（阻断流水线）
powershell -ExecutionPolicy Bypass -File Scripts/validation/validate_no_legacy_automation_entrypoints.ps1
```

---

## 8. CI/CD 架构（Gauntlet）

### 8.1 Gauntlet 在系统中的角色

Gauntlet 是 UE5 官方的**外部测试会话编排框架**（C#，基于 UAT）。在 v0.3 中实际用 C# 编写配置：

```
CI/CD 流水线（Jenkins / GitHub Actions）
    │
    ▼
Gauntlet（C# 编排器）
    │  ← AgentBridge.TestConfig.cs 定义测试会话
    │
    ├── 启动 UE5 Editor 进程（-Unattended -NullRHI）
    ├── 等待 Editor 就绪
    ├── 通过 GauntletTestController 驱动测试
    │   └── Controller 在 Editor 内调用 Automation RunTests
    ├── 监控进程健康（超时 / 崩溃检测）
    ├── 收集测试结果（Saved/Automation/）
    ├── 判定通过/失败
    └── 停止 Editor 进程
```

### 8.2 Gauntlet vs Orchestrator 的关系

| 维度 | Orchestrator | Gauntlet |
|---|---|---|
| 编排目标 | 开发操作（Spec → Actor → 验证） | 测试会话（启动 → 运行 → 收集 → 判定） |
| 运行位置 | Editor 内部（Commandlet / Python） | Editor 外部（C# / UAT） |
| 进程管理 | 假设 Editor 已启动 | **自动启动/监控/停止 Editor** |
| 超时 | Orchestrator 级别 | 会话级别（MaxDuration） |
| 崩溃恢复 | 记录断点 | **自动重启 Editor** |

两者不是替代关系：**Gauntlet 负责编排"在什么环境下运行"，Orchestrator 负责编排"运行什么内容"**。

---

## 9. 本方案在 UE5 官方体系中的位置

```
┌─────────────────────────────────────────────────────────────┐
│  UE5 官方引擎能力                                           │
│                                                             │
│  ┌──────────────┐ ┌───────────────┐ ┌────────────────────┐ │
│  │ Editor       │ │ Automation    │ │ Build / CI         │ │
│  │              │ │               │ │                    │ │
│  │ Level Editor │ │ Test Framework│ │ UAT                │ │
│  │ Asset Browser│ │ Spec (BDD)   │ │ Commandlet         │ │
│  │ Blueprint Ed │ │ Functional   │ │ Gauntlet           │ │
│  │ Python Script│ │ Driver(排除) │ │                    │ │
│  │ RC API       │ │              │ │                    │ │
│  └──────┬───────┘ └──────┬───────┘ └─────────┬──────────┘ │
│         │                │                    │            │
│  ═══════▼════════════════▼════════════════════▼════════    │
│                                                             │
│         AgentBridge C++ Plugin（本方案）                     │
│         ══════════════════════════════                       │
│         在 UE5 官方能力之上增加：                             │
│         • 结构化工具接口（参数校验 + 统一响应 + Schema）     │
│         • 执行护栏（dry_run + 白名单 + 只读查询分离）       │
│         • 验证闭环（写后读回 + 容差比对 + mismatch 检测）    │
│         • 审计能力（结构化报告 + 操作日志 + dirty 追踪）     │
│         • Transaction 管理（Undo/Redo 自动纳入）            │
│         • Spec 驱动编排（YAML Spec → 自动执行）             │
│                                                             │
│  ═══════▲════════════════════════════════════════════════    │
│         │                                                   │
│         对外暴露方式：                                       │
│         • RC API 端点（通道 B → Agent / MCP / Python）      │
│         • Python unreal 模块（通道 A → 脚本）               │
│         • C++ API（通道 C → 引擎内直接调用）                │
│         • Commandlet（→ CI/CD 无头执行）                    │
│         • UAT（→ 构建自动化）                               │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 10. 与其他文档的关系

| 文档 | 职责 |
|---|---|
| **本文档** | 系统架构全图（C++ Plugin 中心） |
| `ue5_capability_map.md` | 10 个 UE5 官方模块详细说明 + 与本方案的映射 |
| `bridge_implementation_plan.md` | AgentBridge Plugin 的详细实现方案（C++ 代码结构） |
| `mvp_smoke_test_plan.md` | 测试层（Automation Test / Spec / Functional Testing）详细方案 |
| `orchestrator_design.md` | Orchestrator 编排逻辑 + Gauntlet 集成 |
| `tool_contract_v0_1.md` | 15 个工具的参数、响应、UE5 依赖契约 |
| `AGENTS.md` | Agent 行为规则（如何调用工具、遵守什么约束） |

> 完整的 v0.3 文件清单参见 `v0.3_file_manifest.md`。

