# MVP Scope

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 文档目的

本文档明确当前 AGENT + UE5 可操作层 MVP 的边界，防止在早期阶段过度扩张范围。

---

## 2. MVP 目标

MVP 的核心目标是建立一条最小可用闭环：

1. 用户 / 设计文档输入
2. 结构化 Spec 生成（Agent 内部完成）
3. Agent 选择工具执行
4. UE5 Bridge 调用 UE5.5.4 Editor 能力
5. 反馈接口读回结果
6. 验证层确认结果（actual vs expected 逐字段比对）
7. 输出结构化报告

当前最高优先级：**先跑通本地 schema/example/validate 校验链，再进入 Python Bridge 最小闭环实现。**

---

## 3. MVP 包含内容

### 3.1 文档与规则

- `AGENTS.md`（Agent 行为规则）
- `Docs/tool_contract_v0_1.md`（工具契约，含 Args/Response）
- `Docs/field_specification_v0_1.md`（字段规范）
- `Docs/feedback_interface_catalog.md`（反馈接口清单）
- `Docs/feedback_write_mapping.md`（写-读-验映射表）
- `Docs/architecture_overview.md`（总体架构）
- `Specs/` 目录（结构化 Spec 模板）

### 3.2 核心反馈接口（7 个）

| 接口 | 用途 |
|---|---|
| `get_current_project_state` | 项目与编辑器上下文 |
| `list_level_actors` | 关卡 Actor 列表 |
| `get_actor_state` | Actor transform / collision / tags |
| `get_actor_bounds` | Actor 世界包围盒 |
| `get_asset_metadata` | 资产元数据与存在性 |
| `get_dirty_assets` | 未保存脏资产列表 |
| `run_map_check` | 地图健康检查 |

辅助接口：`get_editor_log_tail`（日志读取，Schema 暂未单独定义）

### 3.3 核心写接口（4 个）

| 接口 | 风险 | 必须返回 |
|---|---|---|
| `import_assets` | 低-中 | created_objects + dirty_assets |
| `create_blueprint_child` | 中 | created_objects + dirty_assets |
| `spawn_actor` | 中 | actual_transform + dirty_assets |
| `set_actor_transform` | 中 | old_transform + actual_transform + dirty_assets |

### 3.4 MVP Phase 2 接口

以下接口计划在 MVP 后半段实现，当前 Tool Contract 已给出设计方向但部分反馈接口待补充：

写接口：`set_actor_collision` / `assign_material`

验证接口：`validate_actor_inside_bounds` / `validate_actor_non_overlap`

产物采集：`capture_viewport_screenshot`（不做自动判定）

反馈接口待补充：`get_material_assignment` / `get_component_state`

### 3.5 Schema 与校验链

- 6 个 common Schema（primitives / transform / bounds / collision / error / material）
- 7 个 feedback Schema + 1 个 write_feedback Schema
- 8 个 example JSON
- `validate_examples.py` 校验脚本（已跑通，8/8 通过）
- `v0.1_manifest.json` 版本清单

### 3.6 验证能力

- 写后读回（写接口自身 actual_* + 独立反馈接口二次确认）
- 容差比对（location ≤ 0.01cm / rotation ≤ 0.01° / scale ≤ 0.001）
- dirty assets 副作用确认
- map check 地图健康检查
- 结构化状态输出（success / warning / failed / mismatch / validation_error）

---

## 4. MVP 不包含内容

以下内容不属于当前 MVP：

- Blueprint 图深度重写（Python Editor Scripting 不支持操作 K2Node——技术限制）
- Niagara 图编辑
- 材质图深度修改
- 动画蓝图状态机编辑
- Sequencer 深层编辑
- 多 Agent 并发调度
- 自动审美优化
- 大规模整图生成
- 无人监管的高风险批量操作
- 截图自动对比判定（UE5 有 Screenshot Comparison Tool，v0.2 对接）
- 约束自动求值（get_constraint_evaluation）
- 性能快照分析（get_performance_snapshot）
- 完整 Rollback / Checkpoint 机制（UE5 Transaction System 的基础 Undo 能力 MVP 可利用）
- 独立部署的 Spec Translator 组件
- 平台化 / 多项目复用
- 不受控的 GUI 自动化（Automation Driver 已纳入受控框架，但非默认执行路径）

### 4.5 与 UE5 官方能力的关系

AGENT + UE5 可操作层不是替代 UE5 官方能力的自定义系统，而是将 UE5 官方能力统一收编并增加编排层护栏。

**MVP 阶段对 UE5 官方能力的使用策略**：

| UE5 官方能力 | MVP 使用方式 |
|---|---|
| Python Editor Scripting | ✅ Bridge 层通道 A（进程内调用） |
| Remote Control API | ✅ Bridge 层通道 B（Agent 远程调用） |
| Commandlet / CLI | 部分使用（`run_automation_tests` 的 CI 入口） |
| UAT | `build_project` 专用 |
| Automation Test Framework | `run_automation_tests` 工具对接；**v0.3 已实装** L1/L2 测试注册 |
| Automation Spec | **v0.3 已实装** L2 闭环测试（3 个 .spec.cpp） |
| Functional Testing | **v0.3 已实装** L3 完整 Demo（AFunctionalTest + FTEST_ 地图） |
| Automation Driver | **v0.3 已实装** L3 UI 工具执行后端（3 接口 + 封装层 + L3→L1 交叉比对） |
| Gauntlet | v0.2 CI/CD 编排 |
| Transaction System | ✅ 通过 Remote Control API `generateTransaction` 实现基础 Undo |
| Screenshot Comparison Tool | v0.2 对接截图自动对比 |

**验证层实现状态**：v0.3 已将全部验证逻辑实装于 UE5 原生测试体系：L1 单接口测试为 Simple Automation Test（11 个：Query 7 + Write 4），L3.UITool 为独立测试分组（4 个），L2 闭环验证为 Automation Spec（5 个：ClosedLoop 3 + UITool 2），L3 完整 Demo 为 Functional Testing（AFunctionalTest 子类 + FTEST_ 测试地图）。CI/CD 通过 Gauntlet 编排。Schema 校验（validate_examples.py）是编排层自有能力，UE5 没有对应物，永久保留。

> 完整的 UE5 官方能力总览及分层映射，参见 `Docs/ue5_capability_map.md`。

---

## 5. MVP 成功标准

当满足以下全部条件时，可认为 MVP 成功：

1. **校验链跑通**：`validate_examples.py --strict` 返回 exit code 0，全部 example 通过 Schema 校验
2. **最小执行闭环**：能从结构化 Spec 驱动 UE5.5.4 执行至少一条完整链路（spawn_actor + 读回 + 验证）
3. **写后读回**：至少 spawn_actor 和 set_actor_transform 能返回 actual_transform 并通过二次确认
4. **反馈可用**：至少具备 get_actor_state / get_actor_bounds / get_dirty_assets / run_map_check
5. **报告可追踪**：能输出一份包含计划/执行/读回/验证/dirty_assets 的结构化执行报告

---

## 6. 当前推进优先级

| 顺序 | 任务 | 状态 |
|---|---|---|
| 1 | 本地 schema/example/validate 校验链跑通 | ✅ 已完成 |
| 2 | 确定 MVP 核心接口集合（4 写 + 7 读） | ✅ 已确定 |
| 3 | 落地最小 Python Bridge | ⬜ 下一步 |
| 4 | 跑通 spawn_actor → get_actor_state → get_actor_bounds 最小闭环 | ⬜ |
| 5 | Phase 2 接口实现 | ⬜ |
| 6 | 验证增强（inside_bounds / non_overlap / screenshot） | ⬜ |
| 7 | MVP 集成演示 | ⬜ |

---

## 7. 后续版本方向

MVP 之后可考虑扩展：

**v0.3 已完成**（对接 UE5 官方能力）：
- ✅ Bridge 核心实现为 C++ Editor Plugin（AgentBridge Plugin）
- ✅ L1/L2 测试注册为 UE5 Automation Test / Automation Spec
- ✅ L3 完整 Demo 为 Functional Testing（AFunctionalTest + FTEST_ 地图）
- ✅ Remote Control API 作为 Agent→Editor 标准通信层
- ✅ Gauntlet + UAT 实现 CI/CD 无人值守测试编排
- ✅ Commandlet 无头执行（Spec / 测试 / 单工具）
- ✅ FScopedTransaction 原生 Undo/Redo

**后续功能扩展**：
- 组件级反馈增强（get_component_state / get_material_assignment）
- 更完整的碰撞 / 导航接口
- 约束自动求值
- Blueprint 图的安全子集编辑
- 多 Agent 协作
- 独立 Spec Translator
- UE5 Screenshot Comparison Tool 截图自动对比

