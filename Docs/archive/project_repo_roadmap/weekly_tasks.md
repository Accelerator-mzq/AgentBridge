# 周任务清单

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3

## 一、周任务总表

| 周次 | 阶段 | 本周目标 | 关键交付物 | 前置依赖 | 验收标准 | 优先级 |
|---|---|---|---|---|---|---|
| 第 1 周 | 基础 + 校验链 | 搭骨架 + 落规则 + 校验链跑通 | 目录 + 规则文档 + 14 Schema + 8 example + validate 零错误 | 无 | `validate_examples.py --strict` exit code 0 | P0 |
| 第 2 周 | **C++ Plugin 核心** | 创建 AgentBridge Plugin + 查询接口 | Plugin 骨架 + BridgeTypes.h + 7 个查询接口 C++ 实现 | 第 1 周校验链跑通 | Session Frontend 可见；RC API 可调用 Subsystem | P0 |
| 第 3 周 | **写接口 + Transaction** | 4 个写接口 C++ 实现 + FScopedTransaction | 4 个写接口 + 写后读回 + Undo 验证 | 第 2 周查询接口可用 | SpawnActor → Ctrl+Z → Actor 消失 | P0 |
| 第 4 周 | **AgentBridgeTests + L3 UI 工具** | L1（11 个）+ L3.UITool（4 个）+ L2（5 个）+ L3 Automation Driver 封装 | AgentBridgeTests Plugin + TASK 20 L3 UI 工具 | 第 3 周写接口完成 | Session Frontend 20 个测试全部绿灯 | P0 |
| 第 5 周 | **Commandlet + UAT** | 无头执行 + 构建自动化 | AgentBridgeCommandlet + FUATRunner | 第 4 周测试通过 | `-run=AgentBridge -Tool=ListLevelActors` 可执行 | P0 |
| 第 6 周 | **L3 + Gauntlet** | Functional Test + CI/CD 编排 | L3 FunctionalTestActor + Gauntlet C# Config | 第 5 周 Commandlet 可用 | `RunUAT RunGauntlet -Test=AgentBridge.SmokeTests` 通过 | P0 |
| 第 7 周 | Spec 执行链 + Python 客户端 | Orchestrator + Python 三通道 | Spec 模板 + Orchestrator + Python call_cpp_plugin | 第 6 周 Gauntlet 可用 | Spec → C++ Plugin 执行 → 验证报告 | P0 |
| 第 8 周 | 集成 + Demo | 完整 Demo + 文档收束 | 可演示 Demo + v0.3 完整交付 | 前 7 周完成 | Gauntlet AllTests 全部通过 + 完整 Spec 链路 | P0 |

---

## 二、按周拆解

### 第 1 周任务清单

| 任务ID | 任务 | 输出物 | 验收标准 | 优先级 | 状态 |
|---|---|---|---|---|---|
| W1-01 | 创建项目目录结构 | `Docs/` / `Specs/` / `Schemas/` / `Scripts/` / `Artifacts/` / `roadmap/` | 目录完整存在 | P0 | 未开始 |
| W1-02 | 创建根文件 | `AGENTS.md` / `README.md` | 文件存在，包含 UE5.5.4 声明和核心规则 | P0 | 未开始 |
| W1-03 | 编写 MVP 范围文档 | `Docs/mvp_scope.md` | 明确做/不做边界，核心为 4 写 + 7 读 | P0 | 未开始 |
| W1-04 | 编写架构总览文档 | `Docs/architecture_overview.md` | 包含分层架构、C++ Plugin 核心、三通道、9 模块实装 | P1 | 未开始 |
| W1-05 | 编写 Tool Contract 初版 | `Docs/tool_contract_v0_1.md` | 包含核心 15 个接口的 Args/Response 定义 | P0 | 未开始 |
| W1-06 | 初始化 `Schemas/` 目录结构 | `common/` / `feedback/` / `write_feedback/` / `examples/` / `versions/` | 目录完整 | P0 | 未开始 |
| W1-07 | 创建全部 common Schema（实际内容） | `primitives` / `transform` / `bounds` / `collision` / `error` / `material` | 6 个文件存在且 JSON 合法，无 `$id` | P0 | 未开始 |
| W1-08 | 创建首批 feedback + write_feedback Schema | 7 个 feedback + 1 个 write_feedback | 8 个文件存在且 JSON 合法，无 `$id`，`$ref` 可本地解析 | P0 | 未开始 |
| W1-09 | 创建 8 个 example JSON | `Schemas/examples/*.example.json` | JSON 合法，status 值为 `"success"`，字段与 Schema 一致 | P0 | 未开始 |
| W1-10 | 配置 validate_examples.py 映射 | `Scripts/validation/validate_examples.py` | 映射覆盖全部 8 个 example | P0 | 未开始 |
| W1-11 | 运行 validate_examples.py 至零错误 | 脚本输出 | `--strict` 模式：8 checked / 8 passed / 0 failed | P0 | 未开始 |

---

### 第 2 周任务清单（C++ Plugin 核心）[UE5 + C++]

| 任务ID | 任务 | 输出物 | 验收标准 | 优先级 | 状态 |
|---|---|---|---|---|---|
| W2-01 | 创建 AgentBridge Plugin 骨架 | .uplugin + Build.cs + Module.cpp | 编译零 error；Editor 日志 "[AgentBridge] Plugin loaded" | P0 | 未开始 |
| W2-02 | 实现 BridgeTypes.h | 核心类型定义 | EBridgeStatus / FBridgeTransform / FBridgeResponse 等可用 | P0 | 未开始 |
| W2-03 | 实现 `GetCurrentProjectState` | C++ 查询接口 | curl RC API 返回 project_name / engine_version / editor_mode | P0 | 未开始 |
| W2-04 | 实现 `ListLevelActors` | C++ 查询接口 | 返回 actors 数组，含 ClassFilter 过滤 | P0 | 未开始 |
| W2-05 | 实现 `GetActorState` | C++ 查询接口 | 返回 transform/collision/tags；空参数→validation_error；不存在→ACTOR_NOT_FOUND | P0 | 未开始 |
| W2-06 | 实现 `GetActorBounds` | C++ 查询接口 | 返回 world_bounds_origin / world_bounds_extent | P0 | 未开始 |
| W2-07 | 实现 `GetAssetMetadata` | C++ 查询接口 | 返回 exists / class / mesh_asset_bounds | P0 | 未开始 |
| W2-08 | 实现 `GetDirtyAssets` | C++ 查询接口 | 返回 dirty_assets 数组 | P0 | 未开始 |
| W2-09 | 实现 `RunMapCheck` | C++ 查询接口 | 返回 map_errors / map_warnings | P1 | 未开始 |
| W2-10 | 验证 RC API 自动暴露 | curl 测试 | 全部 7 个接口可通过 localhost:30010 调用 | P0 | 未开始 |

---

### 第 3 周任务清单（写接口 + Commandlet + UAT）[UE5 + C++]

| 任务ID | 任务 | 输出物 | 验收标准 | 优先级 | 状态 |
|---|---|---|---|---|---|
| W3-01 | 实现 `SpawnActor` | C++ 写接口 | FScopedTransaction 包裹；写后读回 actual_transform；dry_run 不执行 | P0 | 未开始 |
| W3-02 | 实现 `SetActorTransform` | C++ 写接口 | 读 old → Modify → 写新 → 读回 actual；FScopedTransaction | P0 | 未开始 |
| W3-03 | 实现 `ImportAssets` | C++ 写接口 | 扫描源目录 + ImportAssetTasks + created_objects | P0 | 未开始 |
| W3-04 | 实现 `CreateBlueprintChild` | C++ 写接口 | BlueprintFactory + CreateAsset；不存在父类→CLASS_NOT_FOUND | P0 | 未开始 |
| W3-05 | 实现 3 个验证接口 | C++ 验证接口 | ValidateActorInsideBounds / ValidateActorNonOverlap / RunAutomationTests | P0 | 未开始 |
| W3-06 | 实现 BuildProject + SaveNamedAssets + CaptureViewportScreenshot | C++ 构建+辅助 | UAT 子进程可启动；资产可保存；截图可输出 | P1 | 未开始 |
| W3-07 | 实现 UAgentBridgeCommandlet | Commandlet | -Spec / -RunTests / -Tool 三种模式；退出码 0/1/2 | P0 | 未开始 |
| W3-08 | 实现 FUATRunner | UAT 封装 | BuildCookRun / RunAutomationTests / RunGauntlet 方法可用 | P1 | 未开始 |
| W3-09 | ★ 验证 Transaction/Undo | Undo 测试 | SpawnActor via RC API → Ctrl+Z → Actor 消失 | P0 | 未开始 |
| W3-10 | ★ 验证 Commandlet | 无头测试 | `-run=AgentBridge -Tool=ListLevelActors` 输出 JSON + exit 0 | P0 | 未开始 |

---

### 第 4 周任务清单（AgentBridgeTests + L3 UI 工具 + Python 客户端）[UE5 + C++]

| 任务ID | 任务 | 输出物 | 验收标准 | 优先级 | 状态 |
|---|---|---|---|---|---|
| W4-01 | 创建 AgentBridgeTests Plugin 骨架 | .uplugin + Build.cs + Module.cpp | 编译零 error（含 AutomationDriver 依赖） | P0 | 未开始 |
| W4-02 | 实现 L1 查询测试（7 个） | L1_QueryTests.cpp | Session Frontend 可见 7 个 L1.Query 测试 | P0 | 未开始 |
| W4-03 | 实现 L1 写测试（4 个） | L1_WriteTests.cpp | 含参数校验 + dry_run + 执行 + Transaction + 读回容差 + Undo 清理 | P0 | 未开始 |
| W4-04 | 实现 L2 闭环 Spec（3 个） | L2_ClosedLoopSpecs.spec.cpp | BDD 语法；SpawnReadback / TransformModify / ImportMetadata | P0 | 未开始 |
| W4-05 | 实现 AutomationDriverAdapter 封装层 | .h + .cpp | IsAvailable + ClickDetailPanelButton + TypeInDetailPanelField + DragAssetToViewport | P0 | 未开始 |
| W4-06 | 在 Subsystem 追加 3+1 个 L3 UITool 接口 | AgentBridgeSubsystem.h/.cpp | Category="AgentBridge\|UITool"；含 CrossVerifyUIOperation 交叉比对 | P0 | 未开始 |
| W4-07 | 在 BridgeTypes.h 追加 L3 错误码 + FBridgeUIVerification | BridgeTypes.h | 3 个错误码 + 交叉比对结构体 + 辅助函数 | P0 | 未开始 |
| W4-08 | 实现 L3 UITool 测试（4 个） | L1_UIToolTests.cpp | T1-12~T1-15；含 L3→L1 交叉比对；Driver 不可用时 graceful degradation | P0 | 未开始 |
| W4-09 | 实现 L2 UITool 闭环 Spec（2 个） | L2_UIToolClosedLoopSpec.spec.cpp | DragAssetToViewportLoop(5 It) + TypeInFieldLoop(3 It) | P0 | 未开始 |
| W4-10 | ★ L1+L2+L3.UITool 全部绿灯 | Session Frontend | 20 个测试全部通过（L1×11 + L3.UITool×4 + L2×5） | P0 | 未开始 |
| W4-11 | 确认 Python 三通道客户端 + ui_tools.py | Python 文件 | BridgeChannel 含 CPP_PLUGIN；ui_tools.py Mock 模式 3 个接口返回 success | P0 | 未开始 |
| W4-12 | 测试 Python 通道 C 调用 | 调用测试 | set_channel(CPP_PLUGIN) → get_actor_state → 返回正确 JSON | P0 | 未开始 |

---

### 第 5 周任务清单（Orchestrator 编排）

| 任务ID | 任务 | 输出物 | 验收标准 | 优先级 | 状态 |
|---|---|---|---|---|---|
| W5-01 | 实现 Spec 读取器 | spec_reader.py | read_spec 可读取模板 Spec；validate_spec 返回 (True, []) | P0 | 未开始 |
| W5-02 | 实现计划生成器 | plan_generator.py | 空 actor → 全 CREATE；已有 actor → UPDATE | P0 | 未开始 |
| W5-03 | 实现验证器 | verifier.py | 容差与 C++ NearlyEquals 一致：loc≤0.01, rot≤0.01, scale≤0.001 | P0 | 未开始 |
| W5-04 | 实现报告生成器 | report_generator.py | overall_status 正确；可输出 JSON 文件 | P0 | 未开始 |
| W5-05 | 实现 Orchestrator 主编排 | orchestrator.py | run(spec_path) 默认通道 C → 完整流程 → 报告 | P0 | 未开始 |
| W5-06 | ★ 用模板 Spec 跑通 Orchestrator | 执行报告 | overall_status = success 或 mismatch；单个失败不中断 | P0 | 未开始 |

---

### 第 6 周任务清单（L3 + Gauntlet CI/CD）[UE5 + C++]

| 任务ID | 任务 | 输出物 | 验收标准 | 优先级 | 状态 |
|---|---|---|---|---|---|
| W6-01 | 实现 L3_FunctionalTestActor | .h + .cpp | 继承 AFunctionalTest；内置场景 + Spec 驱动两种模式 | P0 | 未开始 |
| W6-02 | 创建 FTEST_WarehouseDemo 测试地图 | .umap | 放置 AAgentBridgeFunctionalTest Actor | P0 | 未开始 |
| W6-03 | ★ L3 Run Level Test 通过 | Session Frontend | FTEST_WarehouseDemo 地图测试通过 | P0 | 未开始 |
| W6-04 | 编写 Gauntlet C# TestConfig | AgentBridge.TestConfig.cs | AllTests / SmokeTests / SpecExecution 三种配置 | P0 | 未开始 |
| W6-05 | 实现 GauntletTestController | .h + .cpp | OnInit 解析参数 → OnTick 触发+轮询 → EndTest(ExitCode) | P0 | 未开始 |
| W6-06 | ★ Gauntlet SmokeTests 通过 | UAT 退出码 | `RunUAT RunGauntlet -Test=AgentBridge.SmokeTests` exit 0 | P0 | 未开始 |

---

### 第 7 周任务清单（Phase 2 接口 + Schema 扩展）[UE5 + C++]

| 任务ID | 任务 | 输出物 | 验收标准 | 优先级 | 状态 |
|---|---|---|---|---|---|
| W7-01 | 在 Subsystem 追加 `SetActorCollision` | C++ 写接口 | FScopedTransaction；Ctrl+Z 可撤销 | P0 | 未开始 |
| W7-02 | 在 Subsystem 追加 `AssignMaterial` | C++ 写接口 | FScopedTransaction；读回确认材质生效 | P0 | 未开始 |
| W7-03 | 在 Subsystem 追加 `GetComponentState` | C++ 查询接口 | 读取组件碰撞/属性状态 | P1 | 未开始 |
| W7-04 | 在 Subsystem 追加 `GetMaterialAssignment` | C++ 查询接口 | 读取材质槽位分配 | P1 | 未开始 |
| W7-05 | 更新 Python 客户端 _CPP_MAP | Python 修改 | 新增接口在通道 C 可调用 | P0 | 未开始 |
| W7-06 | 为 Phase 2 创建 Schema + example | JSON 文件 | 新增 2 个 Schema + 2 个 example | P0 | 未开始 |
| W7-07 | ★ validate_examples.py --strict 全量通过 | 校验结果 | 从 8/8 扩展为 10/10 | P0 | 未开始 |

---

### 第 8 周任务清单（完整 Demo 端到端）[UE5 + C++]

| 任务ID | 任务 | 输出物 | 验收标准 | 优先级 | 状态 |
|---|---|---|---|---|---|
| W8-01 | 编写真实 Demo Spec | demo_warehouse.yaml | 覆盖 10 个 Actor（spawn / transform / collision / material） | P0 | 未开始 |
| W8-02 | Schema 校验 | 校验结果 | validate_examples.py --strict → 10/10 | P0 | 未开始 |
| W8-03 | L1+L2+L3.UITool 全部绿灯 | Session Frontend | 20 个测试全部绿灯（L1×11 + L3.UITool×4 + L2×5） | P0 | 未开始 |
| W8-04 | L3 Functional Test 通过 | Session Frontend | FTEST_WarehouseDemo → 通过 | P0 | 未开始 |
| W8-05 | Orchestrator Demo 通过 | 执行报告 | 10 Actor Demo → overall_status = success | P0 | 未开始 |
| W8-06 | Commandlet 无头执行 | 退出码 | -run=AgentBridge -Spec=demo.yaml → exit 0 | P0 | 未开始 |
| W8-07 | Gauntlet AllTests 通过 | UAT 退出码 | RunUAT RunGauntlet -Test=AgentBridge.AllTests → exit 0 | P0 | 未开始 |
| W8-08 | 三通道一致性 | 比对结果 | 通道 A/B/C 对同一 Actor 的 get_actor_state 返回值一致（容差内） | P0 | 未开始 |
| W8-09 | 文档收束 | 文档定稿 | 全部文档版本号 v0.3，无过时措辞残留 | P1 | 未开始 |
