# AgentBridge — 通用 Agent 规则

> 插件版本：v0.4.0 | 目标引擎版本：UE5.5.4
>
> 本文件定义使用 AgentBridge 插件时的**通用 Agent 行为规则**。
> 这些规则适用于所有集成 AgentBridge 插件的 UE5 项目。
> 项目可以在项目根目录的 `AGENTS.md` 中添加项目特定的规则覆盖或补充。

---

## 1. 目的

AgentBridge 是一套面向不同 UE5 项目的**通用 Agent 开发框架插件**。它包含：

- **Skill Compiler Plane**：从设计输入和项目现状编译出结构化图纸（`Scripts/compiler/`）
- **Reviewed Handoff**：Compiler 向 Orchestrator 的正式交接物
- **Execution Orchestrator Plane**：基于 Run Plan 的执行编排（`Scripts/orchestrator/`）
- **受控工具体系**：L1 语义工具 > L2 编辑器服务工具 > L3 UI 工具（`Source/AgentBridge/`）
- **验证闭环**：写后读回 + Schema 校验 + 回归验证（`Scripts/validation/`）

**分层原则**：项目层提供输入和实例，插件层提供通用编译与执行机制。

Agent 不得直接执行不可控的 UE5 编辑器 GUI 操作。主干执行路径是受控工具路线（结构化参数 → 确定性 API → 可读回验证）。

所有 UE5 操作都必须通过已批准的受控工具完成。

---

## 2. 核心规则

### 2.1 Spec 优先

Agent 不得直接根据用户自然语言执行 UE5 修改。所有执行都必须来自结构化 Spec。

### 2.2 禁止模糊执行字段

以下字段不得进入执行层：

`size` / `position` / `center` / `middle` / `near` / `far` / `big` / `small` / `normal` / `proper` / `suitable` / `slightly_left` / `a_bit_bigger` / `good_distance` / `nice_spacing` / `proper_scale` / `not_too_close` / `large_enough` / `visually correct` / `looks good`

执行层必须使用明确字段（这些字段名直接映射自 UE5 官方 API 属性名）：

- `location`（世界坐标，cm，`[x, y, z]`）— 对应 `AActor::GetActorLocation()` 的 `FVector`
- `rotation`（旋转，degrees，`[pitch, yaw, roll]`）— 对应 `AActor::GetActorRotation()` 的 `FRotator`
- `relative_scale3d`（缩放倍率，`[x, y, z]`）— 对应 `AActor::GetActorScale3D()` 的 `FVector`
- `world_bounds_origin` / `world_bounds_extent`（包围盒，cm）— 对应 `AActor::GetActorBounds()`
- `collision_box_extent`（碰撞体半径，cm，仅 BoxComponent）— 对应 `UBoxComponent::GetUnscaledBoxExtent()`
- `collision_profile_name`（碰撞预设名）— 对应 `UPrimitiveComponent::GetCollisionProfileName()`
- `can_affect_navigation`（是否影响导航，boolean）

### 2.3 写前必须查询

执行任何写操作前，必须先查询当前项目 / 地图 / 对象状态。

### 2.4 写后必须读回

所有写操作必须读回实际结果并进行验证。读回验证分两步：

1. 写接口自身返回 `actual_*` 字段（第一次读回）
2. 调用独立反馈接口做二次确认（第二次读回）

通过 Remote Control API（通道 B）执行的写操作，应设置 `generateTransaction: true` 将操作纳入 UE5 的 Undo/Redo 事务系统。通过 C++ Plugin（通道 C，推荐）执行的写操作，由 `FScopedTransaction` 自动纳入 Undo——无需额外参数，作用域结束时自动提交，异常时自动回滚。这使得 mismatch 或异常时可通过 UE5 原生 Undo 回滚。

### 2.5 高风险操作必须 dry-run

高风险工具必须先执行 dry-run，再决定是否 apply。

### 2.6 不得猜测关键值

如果关键执行值无法唯一确定，必须：

1. 停止执行
2. 请求澄清
3. 或使用已声明的默认规则（默认规则必须记录在 Spec 或字段规范中）

### 2.7 结果状态必须可判定

所有写操作和反馈接口的返回必须包含 `status` 字段，枚举值为：

| status | 含义 |
|---|---|
| `success` | 执行成功，验证通过 |
| `warning` | 执行成功，有非阻塞警告 |
| `failed` | 执行失败 |
| `mismatch` | 执行成功但读回值与预期不符 |
| `validation_error` | 参数/Spec 校验未通过，未执行 |

在没有读回和验证的情况下，不得报告"完成"。

---

## 3. Agent 的职责

Agent 可以：

- 读取 Spec 并生成执行计划
- 调用已批准的查询 / 写 / 验证工具
- 分析反馈数据并判断是否达成预期
- 在 mismatch 时决定修正策略
- 生成结构化执行报告

Agent 不得：

- 直接操作 UE5 Editor GUI
- 随意重写复杂 Blueprint 图
- 做整图级破坏性编辑
- 在没有反馈闭环的情况下声明"完成"
- 使用模糊字段作为执行参数
- 调用未在 Tool Contract 中定义的工具

---

## 4. 工具使用规则

### 4.1 L1 语义工具（Semantic Tools）— 默认主干

L1 是优先级最高的工具类型。Agent 必须首先尝试 L1。

**L1 查询工具**（默认安全）：
- `get_current_project_state`
- `list_level_actors`
- `get_actor_state`
- `get_actor_bounds`
- `get_asset_metadata`
- `get_dirty_assets`

**L1 写工具**（必须结合反馈接口使用，全部 FScopedTransaction）：
- `import_assets`（低风险）
- `create_blueprint_child`（中风险）
- `spawn_actor`（中风险，必须返回 actual_transform）
- `set_actor_transform`（中风险，必须返回 old_transform + actual_transform）

**L1 Phase 2 写工具**（需要额外反馈接口支撑）：
- `set_actor_collision`（中风险，依赖 get_actor_state 读回完整 collision）
- `assign_material`（中风险，依赖 get_material_assignment）

**L1 验证工具**：
- `validate_actor_inside_bounds`
- `validate_actor_non_overlap`

### 4.2 L2 编辑器服务工具（Editor Service Tools）— 工程服务

L2 提供构建、测试、验证、保存、截图等工程级服务。不直接修改场景内容。

- `run_map_check` — Console Command "MAP CHECK"
- `run_automation_tests` — UE5 Automation Test Framework
- `build_project` — UAT BuildCookRun（Editor 外部 C# 程序）
- `save_named_assets` — 保存脏资产
- `capture_viewport_screenshot` — Viewport 截图采集

### 4.3 L3 UI 工具（UI Tools）— 受约束补充路径

L3 通过 Automation Driver 模拟用户输入。**仅当 L1 无法覆盖时使用。**

- `click_detail_panel_button` — 在 Detail Panel 中点击按钮
- `type_in_detail_panel_field` — 在属性输入框中输入值
- `drag_asset_to_viewport` — 将资产从 Content Browser 拖拽到 Viewport

**L3 使用规则**：
1. Agent 必须先确认 L1 无对应能力，才允许使用 L3
2. 每次 L3 操作后，必须通过 L1 工具做独立读回验证
3. L3 返回值与 L1 验证返回值做交叉比对——两者一致才判定 success
4. Spec 中必须显式标注 `execution_method: ui_tool`
5. 高风险操作（批量删除、不可逆修改）禁止通过 L3 执行

### 4.4 后续扩展工具（当前不可用，Tool Contract 未定义）

以下工具在当前版本不可使用，Agent 不得调用：

`replace_selected_actors` / `batch_rename_assets` / `delete_assets` / `bulk_replace_references` / `modify_many_level_actors` / `rewrite_blueprint_graph` / `package_game`

---

## 5. 默认执行流程

所有 UE5 任务默认遵循以下 10 步：

1. 读取输入
2. 生成或读取结构化 Spec
3. 校验 Spec（拒绝模糊字段、检查引用完整性、检查约束一致性）
4. 查询当前状态
5. 生成执行计划
6. dry-run（如需要）
7. 通过批准工具执行（apply）
8. 读回反馈（写接口自身 actual_* + 独立反馈接口二次确认）
9. 验证（将读回值与预期值逐字段比对）
10. 生成报告

不得跳过第 8-10 步。

---

## 6. 读回验证规则

### 6.1 最低读回字段

对于 Actor 写操作，写后至少读回：

- `actual_transform.location`
- `actual_transform.rotation`
- `actual_transform.relative_scale3d`

如涉及碰撞，还必须读回：

- `collision.collision_profile_name`
- `collision.collision_box_extent`（BoxComponent 场景）
- `collision.can_affect_navigation`

如涉及占地，还必须读回：

- `world_bounds_origin`
- `world_bounds_extent`

### 6.2 容差策略

UE5.5.4 环境下存在浮点精度差异，验证时应采用容差比对：

| 字段 | 建议容差 | 说明 |
|---|---|---|
| location | ≤ 0.01 cm | 浮点精度 |
| rotation | ≤ 0.01 degrees | 浮点精度 |
| relative_scale3d | ≤ 0.001 | 浮点精度 |
| world_bounds_extent | ≤ 1.0 cm | 包围盒计算可能有微小差异 |

超出容差的偏差应标记为 `mismatch`。

### 6.3 读回失败处理

如果读回失败（反馈接口返回 `failed` 或 `READBACK_FAILED`），应视为操作未完成，不得报告"成功"。

---

## 7. Spec 校验规则

执行前，Agent 必须校验：

- **字段命名**：拒绝禁用清单中的模糊字段
- **引用完整性**：所有引用的锚点、地图、资产和 Actor 必须存在
- **约束一致性**：拒绝不可能同时满足的约束组合（如 must_not_overlap + forced_same_location）
- **默认值来源**：所有推断值必须来自已声明的默认规则，不得在执行时临时发明

---

## 8. 报告规则

Agent 必须在执行后报告：

- 原计划是什么（Spec 中的预期）
- 实际执行了什么（调用了哪些工具）
- 实际创建 / 修改了什么（created_objects / modified_objects）
- 读回了哪些值（actual_transform / actual_bounds 等）
- 哪些验证通过 / 失败（逐字段 pass/fail）
- 产生了哪些 dirty assets
- 还存在什么 warnings / errors

---

## 9. 执行通道与工具使用规则

### 通道选择

| 通道 | 方式 | 何时使用 |
|---|---|---|
| 通道 C（推荐） | C++ Plugin via Remote Control API | Agent 远程调用、生产环境、CI/CD |
| 通道 A | Python `import unreal` 进程内 | 快速原型、脚本开发 |
| 通道 B | Remote Control API 直接调用 UE5 原生 API | 无 Plugin 时 fallback |

### Commandlet 使用规则

- Commandlet 用于无头（headless）批处理执行，不应在交互式 Editor 中使用
- 支持三种模式：`-Spec=` 执行 Spec / `-RunTests=` 运行测试 / `-Tool=` 执行单工具
- 退出码规范：0=成功，1=mismatch/warning，2=failed/参数错误
- CI/CD 脚本应根据退出码判定流水线状态

### UAT 使用规则

- UAT 运行在引擎进程**外部**（C# 程序），通过子进程调用
- `BuildProject` 工具通过 UAT 的 `BuildCookRun` 命令实现
- Agent 调用 `build_project` 时应预期较长执行时间（分钟级）
- 构建失败不应视为 Bridge 层错误——应检查 UAT 日志

### Gauntlet 使用规则

- Gauntlet 负责 CI/CD 测试会话编排（启动 Editor → 运行测试 → 收集结果 → 停止 Editor）
- Agent 不直接调用 Gauntlet——Gauntlet 由 CI/CD 流水线触发
- 三种预定义配置：AllTests（每日构建）/ SmokeTests（每次提交）/ SpecExecution（验证特定 Spec）

### L3 UI 工具使用规则

**优先级**：L1 语义工具 > L2 编辑器服务工具 > L3 UI 工具。Agent 必须先尝试 L1。

**允许使用 L3 的判定条件**（全部满足）：
1. L1 无对应 API——已确认 Subsystem 没有能完成该操作的 BlueprintCallable 接口
2. 操作可结构化——可用路径/名称/标签描述，不是屏幕坐标
3. 结果可验证——有对应的 L1 读回接口验证状态变更
4. 操作可逆或低风险——可 Undo 撤销，或操作本身不涉及不可逆数据删除
5. Spec 显式标注——`execution_method: ui_tool`
6. 已封装为接口——通过 AgentBridge Subsystem 的 UITool 接口调用，不直接构造 Automation Driver 命令

**L3→L1 交叉比对**：每次 L3 操作后，必须通过 L1 工具做独立读回。L3 返回值与 L1 返回值做对比——两者一致才判定 success，不一致则为 mismatch（含字段级差异列表）。

**禁止事项**：
- 禁止通过 L3 执行高风险操作（批量删除、不可逆修改）
- 禁止 Agent 直接构造 Automation Driver 低级命令
- 禁止将 L3 作为默认执行路径——必须先证明 L1 无能力

---

## 10. 能力边界（非目标）

当前版本不支持稳定自动化处理以下内容：

- 复杂 Blueprint 图重写（K2Node 的创建/删除/连接不是 BlueprintCallable——L3 UI 工具可在未来通过 Automation Driver 覆盖简单节点连接，但当前不支持复杂图重写）
- Niagara 图修改
- 材质图节点编辑
- 动画蓝图状态机编辑
- Sequencer 深层轨道操作
- 大规模无人监管的场景改造
- 多 Agent 并发调度
- 自动审美优化
- 无边界 GUI 自动化（L3 UI 工具已纳入受控框架用于特定 UI 操作，但 Agent 直接发送屏幕坐标级指令、绕过 Spec 和 AgentBridge 的无约束 GUI 操控不在支持范围内）

这些需要更严格的人工 review 或后续工具能力。

> 完整的 UE5 官方能力总览及与本插件的映射关系，参见 `Docs/ue5_capability_map.md`。

---

## 11. 默认行为总结

当 Agent 不确定时，应：

1. 把请求规范化为结构化 Spec
2. **按 L1 > L2 > L3 优先级选择工具**（默认使用 L1 语义工具）
3. 查询当前状态
4. 拒绝模糊执行
5. 优先选择可回滚的操作（L1 通道 C 的 FScopedTransaction 自动 Undo）
6. 读回实际结果
7. 运行验证（如果使用了 L3，必须做 L3→L1 交叉比对）
8. 精确报告
