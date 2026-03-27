# AGENT + UE5 可操作层 实施路线图

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3

## 总体目标

8 周结束时，系统至少达到：

1. 本地 schema/example/validate 校验链稳定跑通
2. 用户需求可转成结构化 Spec
3. Agent 能通过受控工具操作 UE5.5.4 Editor
4. 4 个核心写接口全部有反馈闭环
5. 能输出结构化执行报告
6. 形成完整的规则层 + Schema 层 + Bridge 层 + 验证层

---

## 阶段总览

| 阶段 | 周次 | 目标 |
|---|---|---|
| Phase 1：基础 + 校验链 | 第 1-2 周 | 规则定型 + **校验链跑通** + 反馈接口 Schema/example + Bridge 雏形 |
| Phase 2：最小写能力 | 第 3-4 周 | 4 个核心写接口 + Spec 驱动执行 |
| Phase 3：验证增强 | 第 5-6 周 | 布局验证 + 碰撞/材质（Phase 2 接口）+ 截图 |
| Phase 4：收束 | 第 7-8 周 | 校验扩展 + 报告统一 + 集成演示 |

---

## 第 1 周：项目骨架 + 规则定型 + 校验链跑通

### 本周目标

搭好项目骨架，落地核心规则文档，**跑通本地 schema/example/validate 校验链**。这是第一个里程碑——后续所有 Bridge 开发都以 Schema 作为"返回值合同"。

### 关键任务

1. 建立仓库目录结构（Docs / Specs / Schemas / Scripts / Artifacts）
2. 落地核心文档初稿：`AGENTS.md` / `tool_contract_v0_1.md` / `mvp_scope.md` / `architecture_overview.md`
3. 创建全部 common Schema（primitives / transform / bounds / collision / error / material）——**实际内容，不是空骨架**
4. 创建首批 feedback Schema（7 个）+ write_feedback Schema（1 个）
5. 创建对应 8 个 example JSON（status 使用 `"success"`）
6. 所有 Schema 不使用 `$id`
7. 运行 `validate_examples.py --strict` 至零错误

### 本周产出

- 项目目录结构
- `AGENTS.md` / `tool_contract_v0_1.md` / `mvp_scope.md` / `architecture_overview.md`
- 14 个 Schema 文件（6 common + 7 feedback + 1 write_feedback）
- 8 个 example JSON
- `validate_examples.py` 校验脚本
- `v0.1_manifest.json`

### 验收标准

- `validate_examples.py --strict` 返回 exit code 0（8 passed / 0 failed）
- 新人打开仓库能看懂系统骨架
- 做/不做边界明确

---

## 第 2 周：C++ Plugin 核心 + 反馈接口

### 本周目标

创建 AgentBridge C++ Editor Plugin 骨架，实现 BridgeTypes.h 核心类型和 7 个查询接口。对应 TASK 03-04。

### 关键任务

1. 创建 AgentBridge Plugin 骨架（.uplugin + Build.cs + AgentBridgeModule.cpp）
2. 实现 BridgeTypes.h（EBridgeStatus / EBridgeErrorCode / FBridgeTransform / FBridgeResponse + 辅助函数）
3. 实现 UAgentBridgeSubsystem 的 7 个查询接口（C++ 原生实现）：
   - `GetCurrentProjectState`
   - `ListLevelActors`
   - `GetActorState`
   - `GetActorBounds`
   - `GetAssetMetadata`
   - `GetDirtyAssets`
   - `RunMapCheck`
4. 确认 Remote Control API 自动暴露 Subsystem 的 BlueprintCallable 函数
5. 用 curl 测试每个接口可通过 HTTP 调用

### 本周产出

- AgentBridge C++ Plugin 编译通过并加载
- 7 个查询接口通过 Remote Control API 可调用
- 返回值符合 Schema 定义

### 验收标准

- 编译零 error，Editor 日志出现 "[AgentBridge] Plugin loaded"
- curl http://localhost:30010 可调用全部 7 个查询接口
- GetActorState 空参数返回 validation_error，不存在路径返回 ACTOR_NOT_FOUND

---

## 第 3 周：写接口 + Commandlet + UAT

### 本周目标

在 C++ Plugin 中实现 4 个写接口（FScopedTransaction 原生 Undo）+ 3 个验证接口 + Commandlet 无头执行 + UAT 封装。对应 TASK 05-06。

### 关键任务

1. 实现 `SpawnActor`（FScopedTransaction + 写后读回 FBridgeTransform::FromActor）
2. 实现 `SetActorTransform`（读 old → Modify → 写新 → 读回 actual）
3. 实现 `ImportAssets` / `CreateBlueprintChild`
4. 实现 `ValidateActorInsideBounds` / `ValidateActorNonOverlap` / `RunAutomationTests`
5. 实现 `BuildProject`（UAT 子进程）
6. 实现 `UAgentBridgeCommandlet`（-Spec / -RunTests / -Tool 三种模式）
7. 实现 `FUATRunner`（BuildCookRun / RunAutomationTests / RunGauntlet）
8. 验证 Undo：SpawnActor → Ctrl+Z → Actor 消失

### 本周产出

- 全部 15 个 Bridge 接口在 C++ Plugin 中实装
- Commandlet 无头执行可用
- UAT 子进程封装可用

### 验收标准

- SpawnActor → Ctrl+Z → Actor 消失（FScopedTransaction 生效）
- dry_run 不修改 World
- Commandlet: `-run=AgentBridge -Tool=ListLevelActors` 输出 JSON + exit code 0

---

## 第 4 周：AgentBridgeTests Plugin + L3 UI 工具 + Python 三通道客户端

### 本周目标

创建测试 Plugin（L1 × 11 + L3.UITool × 4 + L2 × 5），实现 L3 Automation Driver 封装层，更新 Python 层为三通道客户端。对应 TASK 07-09 + TASK 20。

### 关键任务

1. 创建 AgentBridgeTests Plugin 骨架
2. 实现 L1 查询测试（7 个 Simple Automation Test）
3. 实现 L1 写测试（4 个 Simple Automation Test，含 Undo 清理）
4. 实现 L2 闭环验证（3 个 Automation Spec：SpawnReadback / TransformModify / ImportMetadata）
5. **实现 L3 Automation Driver 封装层**（AutomationDriverAdapter.h/.cpp）
6. **实现 3 个 L3 UI 工具接口**（ClickDetailPanelButton / TypeInDetailPanelField / DragAssetToViewport）
7. **实现 L3 UITool 测试（4 个）+ L2 UITool 闭环（2 个 Spec）**
8. 确认 Python 三通道客户端 + ui_tools.py

### 本周产出

- AgentBridgeTests Plugin 编译通过
- 15 个 L1 测试 + 5 个 L2 Spec 在 Session Frontend 可见（含 L3 UI 工具测试）
- L3→L1 交叉比对（FBridgeUIVerification）可工作
- Python 通道 C（CPP_PLUGIN）可调用 L1/L3 全部接口

### 验收标准

- Session Frontend 中共 20 个测试全部绿灯（L1×11 + L3.UITool×4 + L2×5）
- `Automation RunTests Project.AgentBridge` 通过
- Python `set_channel(BridgeChannel.CPP_PLUGIN)` 后可调用接口

---

## 第 5 周：Orchestrator 编排

### 本周目标

构建完整的 Spec → 计划 → 执行 → 验证 → 报告 编排链，默认通过通道 C 调用 C++ Plugin。对应 TASK 10-14。

### 关键任务

1. 实现 Spec 读取器（YAML 解析 + 格式验证）
2. 实现计划生成器（CREATE / UPDATE 判定）
3. 实现验证器（容差与 C++ FBridgeTransform::NearlyEquals 一致）
4. 实现报告生成器（overall_status + Actor 明细 + dirty_assets）
5. 实现 Orchestrator 主编排（串联全流程，默认 BridgeChannel.CPP_PLUGIN）
6. 用模板 Spec 跑通完整流程

### 本周产出

- 完整可工作的 Orchestrator
- 结构化执行报告

### 验收标准

- `run("Specs/templates/scene_spec_template.yaml")` 执行完成
- 报告 overall_status 为 success 或 mismatch
- 单个 Actor 失败不中断后续执行

---

## 第 6 周：L3 Functional Test + Gauntlet CI/CD

### 本周目标

实现 L3 完整 Demo 测试（FTEST_ 地图）和 Gauntlet CI/CD 编排。对应 TASK 15-16。

### 关键任务

1. 实现 L3_FunctionalTestActor（AFunctionalTest 子类，内置场景 + Spec 驱动两种模式）
2. 创建 FTEST_WarehouseDemo 测试地图，放置测试 Actor
3. 编写 Gauntlet C# TestConfig（AllTests / SmokeTests / SpecExecution 三种配置）
4. 实现 AgentBridgeGauntletController（OnInit 解析参数 → OnTick 触发测试 → EndTest）
5. 测试 Gauntlet CI/CD 流水线

### 本周产出

- L3 Functional Test 在 FTEST_ 地图中可运行
- Gauntlet 三种 CI/CD 配置可用
- 全部三层测试（L1/L2/L3）可通过 Gauntlet 无人值守运行

### 验收标准

- Session Frontend → Run Level Test → FTEST_WarehouseDemo 通过
- `RunUAT RunGauntlet -Test=AgentBridge.SmokeTests` exit code 0

---

## 第 7 周：Phase 2 接口 + Schema 扩展

### 本周目标

追加碰撞/材质写入接口 + 组件反馈接口，扩展 Schema 覆盖。对应 TASK 17-18。

### 关键任务

1. 在 Subsystem 中追加 SetActorCollision / AssignMaterial（FScopedTransaction）
2. 追加 GetComponentState / GetMaterialAssignment
3. 同步更新 Python 客户端 _CPP_MAP
4. 为 Phase 2 接口创建 Schema + example JSON
5. 运行 `validate_examples.py --strict` 确认全量通过（10/10）

### 本周产出

- Phase 2 写接口 + 反馈接口
- 扩展后的 Schema 体系

### 验收标准

- SetActorCollision → Ctrl+Z 可撤销
- AssignMaterial → 读回确认材质生效
- Schema 校验 10/10 通过

---

## 第 8 周：完整 Demo 端到端

### 本周目标

用完整 Demo 验证系统全链路。对应 TASK 19。

### 关键任务

1. 编写真实 Demo Spec（如仓库布局 10 个 Actor）
2. Schema 校验：validate_examples.py --strict → 全部通过
3. L1+L2 Session Frontend → 全部绿灯
4. L3 FTEST_WarehouseDemo → 通过
5. Orchestrator 10 Actor Demo → overall_status = success
6. Commandlet 无头执行 → exit code 0
7. Gauntlet AllTests → exit code 0
8. 三通道一致性验证（通道 A/B/C 对同一 Actor 返回值一致）

### 本周产出

- 完整可演示的 v0.3 交付
- 7 步端到端验证全部通过

### 验收标准

- Schema 10/10 / L1+L2 20 个绿灯 / L3 通过
- Orchestrator success / Commandlet exit 0 / Gauntlet exit 0 / 三通道一致

可以实际演示：**输入 Spec → Agent 规划 → 工具执行 → UE5 修改 → 反馈验证 → 输出报告**

---

## 每周交付物总表

| 周次 | 阶段 | 关键交付物 |
|---|---|---|
| 第 1 周 | 基础 + 校验链 | 目录 + 规则文档 + **14 Schema + 8 example + validate 零错误** |
| 第 2 周 | C++ Plugin 核心 | AgentBridge Plugin + 7 个查询接口（C++ 实装） |
| 第 3 周 | 写接口 + Commandlet | 4 个写接口（FScopedTransaction）+ Commandlet + UATRunner |
| 第 4 周 | 测试 + L3 UI 工具 + Python | L1 × 11 + L3.UITool × 4 + L2 × 5 + Automation Driver 封装层 + Python 三通道 |
| 第 5 周 | Orchestrator | Spec 读取 → 计划 → 执行 → 验证 → 报告（通道 C 默认） |
| 第 6 周 | L3 + Gauntlet | FTEST_ Functional Test + Gauntlet CI/CD 三种配置 |
| 第 7 周 | Phase 2 + Schema | 碰撞/材质接口 + Schema 扩展 10/10 |
| 第 8 周 | 完整 Demo | 7 步端到端验证全部通过 + v0.3 完整交付 |

---

## 接口清单

### 核心反馈接口（7 个，第 2 周实现）

`get_current_project_state` / `list_level_actors` / `get_actor_state` / `get_actor_bounds` / `get_asset_metadata` / `get_dirty_assets` / `run_map_check`

### 核心写接口（4 个，第 3 周实现）

`import_assets` / `create_blueprint_child` / `spawn_actor` / `set_actor_transform`

### Phase 2 接口（第 5-6 周实现）

写：`set_actor_collision` / `assign_material`

验证：`validate_actor_inside_bounds` / `validate_actor_non_overlap`

采集：`capture_viewport_screenshot`

反馈：`get_component_state` / `get_material_assignment`

---

## v0.3 已完成

v0.3 核心升级（全部 10 个 UE5 官方模块实装）：

| 方向 | UE5 官方能力 | 状态 |
|---|---|---|
| C++ Plugin | AgentBridge C++ Editor Plugin（UEditorSubsystem） | ✅ 已实装 |
| 测试体系 | Automation Test Framework + Automation Spec | ✅ L1(11个) + L3.UITool(4个) + L2(5个) 已实装 |
| 关卡验证 | Functional Testing + FTEST_ 地图 | ✅ L3 已实装 |
| CI/CD 编排 | Gauntlet + UAT | ✅ C# TestConfig + GauntletController 已实装 |
| 无头执行 | Commandlet | ✅ AgentBridgeCommandlet 已实装 |
| 构建自动化 | UAT BuildCookRun | ✅ FUATRunner + BuildProject 已实装 |
| 通信标准化 | Remote Control API + MCP Server | ✅ Plugin BlueprintCallable 自动暴露 |

**后续扩展方向**：
- Screenshot Comparison Tool 截图自动对比
- 组件级反馈增强（get_component_state / get_material_assignment）
- 多 Agent 协作 / Spec Translator / 约束自动求值

> 完整的 UE5 官方能力总览，参见 `Docs/ue5_capability_map.md`。
