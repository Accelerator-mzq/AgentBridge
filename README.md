# AgentBridge — UE5 AI Agent 受控编排层插件

> 插件版本：v0.3.0 | 目标引擎版本：UE5.5.4

## 1. 插件定义

**一句话定义**：AgentBridge 是一套位于 AI Agent 与 UE5 官方能力之间的受控编排层插件，通过"结构化 Spec → 受控工具 → 写后读回 → 结构化验证"闭环，让 AI Agent 在可控边界内参与 UE5 开发流程。

**核心定位**：本插件不是 Unreal Engine 内置的单一官方模块，也不是替代 UE5 官方能力的自定义系统。它将分散的 UE5 官方能力（Python Editor Scripting / Remote Control API / Commandlet / UAT / Automation Test Framework 等）统一收编为结构化工具，并对这些工具增加参数约束、权限控制、执行护栏、验证闭环与审计能力。

**本插件不是**让 AI 直接不受控地点击 UE5 编辑器 GUI。**本插件是**把 UE5 官方 API 封装为可被 Agent 安全调用的结构化工具平台。主干路径是受控工具调用（结构化参数 → 确定性 API → 可验证）；对于无直接 API 的 UI 级操作，可通过 Automation Driver 作为受约束的执行后端。

---

## 2. 安装

### 方式 A：直接拷贝

将本插件文件夹拷贝到 UE5 项目的 `Plugins/` 目录下：

```
YourProject/Plugins/AgentBridge/
```

### 方式 B：Git Submodule

```bash
cd YourProject
git submodule add <repository-url> Plugins/AgentBridge
```

### 依赖插件

本插件依赖以下 UE5 官方插件（在 `.uplugin` 中已声明，安装后自动启用）：

- **EditorScriptingUtilities** — Editor 脚本工具
- **RemoteControl** — Remote Control API
- **PythonScriptPlugin** — Python Editor Scripting

### 配合使用

安装插件后，建议在 UE5 项目根目录放置以下文件（插件不包含，需按项目配置）：

- `AGENTS.md` — 项目级 Agent 规则（可引用 `Plugins/AgentBridge/AGENTS.md` 中的通用规则）
- `task.md` — 项目级任务清单

---

## 3. 核心设计路线

### 3.1 三层受控工具体系

AI 通过受控工具调用间接操作 UE5。"受控工具调用"按确定性和风险等级分为三层：

| 层次 | 名称 | 优先级 | 说明 |
|---|---|---|---|
| **L1** | **语义工具** | 最高（默认主干） | 通过 C++ API 直接操作引擎对象，确定性最高 |
| **L2** | **编辑器服务工具** | 中 | 构建/测试/验证/截图等工程服务 |
| **L3** | **UI 工具** | 最低（仅 fallback） | 通过 Automation Driver 模拟 UI 输入，仅当 L1 无能力时使用 |

**优先级规则**：L1 > L2 > L3。Agent 必须先尝试 L1。使用 L3 后必须通过 L1 做独立验证。

可用通道及其 UE5 官方模块：

| 通道 | UE5 官方模块 | 角色 | 状态 |
|---|---|---|---|
| **C（推荐）** | **AgentBridge C++ Plugin** (UEditorSubsystem) | L1/L2/L3 核心实现 | ✅ **v0.3 核心** |
| A | Python Editor Scripting (`unreal` 模块) | L1 进程内执行，快速原型 | ✅ 客户端 |
| B | Remote Control API (HTTP REST) | L1 Agent 远程调用 | ✅ 客户端 |
| — | Commandlet (`UAgentBridgeCommandlet`) | L2 无 GUI 批处理，CI/CD 入口 | ✅ 已实装 |
| — | UAT (BuildCookRun / RunAutomationTests) | L2 构建 / 打包 / CI 编排 | ✅ 已实装 |
| — | Gauntlet (C# TestConfig) | L2 CI/CD 测试会话编排 | ✅ 已实装 |
| — | **Automation Driver** (IAutomationDriverModule) | **L3 UI 工具执行后端** | ✅ 已实装 |

### 3.2 Spec 驱动，不是自然语言直接执行

用户自然语言可以作为输入，但不能直接进入执行层。执行前必须经过结构化 Spec 转换。执行层只接受明确字段：`location` / `rotation` / `relative_scale3d` / `world_bounds_extent` / `collision_box_extent` 等。这些字段名直接映射自 UE5 官方 API 的属性名（如 `location` 对应 `AActor::GetActorLocation()`）。

### 3.3 写操作必须进入闭环

所有写接口都必须绑定反馈接口，形成闭环：

1. 写前查询当前状态
2. 写接口返回 `actual_*`（第一次读回——从 UE5 API 重新读取，不是复制输入参数）
3. 独立反馈接口二次确认（第二次读回）
4. 将读回值与预期值比对，输出可判定状态（`success` / `mismatch` / `failed`）

通过 C++ Plugin（通道 C）执行的写操作由 `FScopedTransaction` 自动纳入 UE5 Undo 系统。通过 Remote Control API（通道 B）可设置 `generateTransaction: true` 达到相同效果。

### 3.4 反馈接口是核心能力

真正决定系统可用性的不只是"能不能写"，而是**能不能读回结果，确认自己写对了**。反馈接口底层调用的是 UE5 原生 Actor/Asset/Package API，Bridge 封装层将其结构化为统一 JSON 格式。

---

## 4. 功能清单

### L1 语义写接口（4 个，FScopedTransaction）

| 写接口 | UE5 依赖 | 验证闭环 |
|---|---|---|
| `import_assets` | `UAssetTools::ImportAssetTasks()` | get_asset_metadata + get_dirty_assets + get_editor_log_tail |
| `create_blueprint_child` | `UBlueprintFactory` + `UAssetTools::CreateAsset()` | get_asset_metadata + get_dirty_assets |
| `spawn_actor` | `UEditorLevelLibrary::SpawnActorFromClass()` | list_level_actors + get_actor_state + get_actor_bounds + get_dirty_assets |
| `set_actor_transform` | `AActor::SetActorLocationAndRotation()` | get_actor_state + get_actor_bounds + get_dirty_assets |

### 核心反馈接口（7 个 + 日志）

`get_current_project_state` / `list_level_actors` / `get_actor_state` / `get_actor_bounds` / `get_asset_metadata` / `get_dirty_assets` / `run_map_check` / `get_editor_log_tail`

### L3 UI 工具（Automation Driver，仅当 L1 无能力时使用）

- `click_detail_panel_button` — 在 Detail Panel 中点击按钮
- `type_in_detail_panel_field` — 在属性输入框中输入值
- `drag_asset_to_viewport` — 将资产从 Content Browser 拖拽到 Viewport

每次 L3 操作后必须通过 L1 做独立读回，L3 返回值与 L1 返回值交叉比对。

### Phase 2 扩展接口（规划中）

- `set_actor_collision` + `assign_material`（L1 写接口）
- `validate_actor_inside_bounds` + `validate_actor_non_overlap` + `capture_viewport_screenshot`（L2 验证接口）
- `get_material_assignment`（L1 反馈接口，Tool Contract 待补充）

### 能力边界（当前不包含）

- Blueprint 图深度重写（K2Node——L3 UI 工具可覆盖简单节点连接，但复杂图重写不支持）
- Niagara 图编辑 / 材质图深度修改 / 动画蓝图状态机编辑 / Sequencer 深层操作
- 多 Agent 并发 / 大规模无人监管场景改造 / 自动审美优化
- 不受控的 GUI 自动化（Automation Driver 已纳入受控框架，但非默认路径）

---

## 5. 插件目录结构

```
AgentBridge/
├── AgentBridge.uplugin              # 插件描述符
├── README.md                        # 本文件
├── AGENTS.md                        # 使用本插件时的通用 Agent 规则
│
├── Source/                          # C++ 源码（核心实现）
│   └── AgentBridge/
│       ├── AgentBridge.Build.cs
│       ├── Private/                 # AgentBridgeModule / Subsystem / Commandlet / AutomationDriverAdapter / UATRunner
│       └── Public/                  # AgentBridgeSubsystem.h / Commandlet.h / BridgeTypes.h 等
│
├── Docs/                            # 设计文档（11 个 .md）
│   ├── ue5_capability_map.md        # ★ UE5 官方能力分层映射（核心参考）
│   ├── architecture_overview.md     # 总体架构
│   ├── mvp_scope.md                 # MVP 边界
│   ├── tool_contract_v0_1.md        # 工具契约（含 UE5 依赖标注）
│   ├── field_specification_v0_1.md  # 字段规范
│   ├── feedback_interface_catalog.md # 反馈接口清单
│   ├── feedback_write_mapping.md    # 写-读映射关系表
│   ├── bridge_implementation_plan.md # Bridge 实现方案
│   ├── mvp_smoke_test_plan.md       # 冒烟测试方案
│   ├── bridge_verification_and_error_handling.md # 接口验证与错误处理
│   └── orchestrator_design.md       # Orchestrator 设计
│
├── Schemas/                         # 数据格式契约（JSON Schema + examples）
│   ├── common/                      # 通用基础类型（6 个）
│   ├── feedback/                    # 反馈接口 Schema（7 个）
│   ├── write_feedback/              # 写后反馈 Schema
│   ├── examples/                    # 示例 JSON（8 个）
│   ├── versions/                    # 版本清单
│   └── README.md
│
├── Scripts/                         # Python 客户端 + 校验脚本
│   ├── bridge/                      # Bridge 封装层（三通道客户端）
│   └── validation/                  # Schema/example 校验脚本
│
├── Specs/                           # 结构化设计 Spec 模板
│   ├── templates/
│   └── README.md
│
├── Gauntlet/                        # CI/CD 测试会话配置
│   └── AgentBridge.TestConfig.cs
│
└── Roadmap/                         # 路线图与周任务
    ├── mvp_roadmap.md
    └── weekly_tasks.md
```

> 测试插件 `AgentBridgeTests` 位于嵌套目录 `Plugins/AgentBridge/AgentBridgeTests/`，按需显式启用。

---

## 6. 推荐阅读顺序

1. 本文件（`README.md`）+ `Docs/mvp_scope.md` — 了解插件定义与边界
2. `Docs/ue5_capability_map.md` — 理解本插件建立在 UE5 哪些官方能力之上
3. `Docs/architecture_overview.md` — 看整体架构
4. `AGENTS.md` + `Docs/tool_contract_v0_1.md` + `Docs/field_specification_v0_1.md` — 看规则与约束
5. `Docs/feedback_interface_catalog.md` + `Docs/feedback_write_mapping.md` — 看接口闭环设计
6. `Docs/bridge_implementation_plan.md` + `Docs/mvp_smoke_test_plan.md` + `Docs/orchestrator_design.md` — 看实现与测试
7. `Schemas/README.md` + `Scripts/validation/validate_examples.py` — 看 Schema 与校验链
8. `Roadmap/mvp_roadmap.md` + `Roadmap/weekly_tasks.md` — 看路线图

---

## 7. 统一口径速查

### 返回状态

| status | 含义 |
|---|---|
| `success` | 执行成功，验证通过 |
| `warning` | 执行成功，有非阻塞警告 |
| `failed` | 执行失败 |
| `mismatch` | 执行成功但读回值与预期不符 |
| `validation_error` | 参数/Spec 校验未通过，未执行 |

### 统一响应外壳

```json
{
  "status": "success",
  "summary": "...",
  "data": { ... },
  "warnings": [],
  "errors": []
}
```

### 禁止进入执行层的模糊字段

`size` / `position` / `center` / `near` / `proper` / `looks good` — 完整清单见 `Docs/field_specification_v0_1.md` 第 6 节。

### 术语区分

- **结构化 Spec**：本方案中的设计文档（scene_spec_template.yaml）
- **Automation Spec**：UE5 官方的 BDD 测试语法（.spec.cpp）

---

## 8. 一句话总结

这套插件的核心不是"让 AI 像人一样使用 UE5 编辑器"，而是：**把 UE5 官方已有的分散能力（Python Editor Scripting / Remote Control API / Commandlet / UAT / Automation Test Framework）统一收编为结构化工具平台，让 Agent 在规则、反馈和验证护栏内调用这些工具。** Bridge 封装层的全部执行能力来自 UE5 官方 API，编排层的价值在于统一接入、参数约束、验证闭环和审计能力。
