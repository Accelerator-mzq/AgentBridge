# 测试方案

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 文档目的

本文档定义 AGENT + UE5 可操作层的测试方案：**基于 UE5 Automation Test Framework 的三层测试体系**，验证系统从工具调用到执行到读回到验证的完整链路。

### 1.1 与 v0.2 的核心区别

| 维度 | v0.2 | v0.3 |
|---|---|---|
| 实现语言 | Python 脚本 | **C++（UE5 Automation Test 宏）** |
| 注册方式 | 手工调用 `python xxx.py` | **引擎自动注册（宏声明）** |
| 执行方式 | 命令行 Python | **Session Frontend UI / RunTests / Commandlet / UAT / Gauntlet** |
| 结果查看 | 控制台文本 | **Session Frontend UI（绿/红状态灯）** |
| 断言机制 | Python assert | **TestEqual / TestNearlyEqual / TestTrue** |
| CI/CD 集成 | 无 | **Gauntlet 原生支持** |
| 测试过滤 | 无 | **Test Flag（Smoke / Product / Editor）** |

### 1.2 保留 Python 的部分

Schema 校验（`validate_examples.py`）保留在 Python 端——UE5 没有 JSON Schema 对应物，这是编排层自有能力。

---

## 2. 三层测试体系

| 层次 | UE5 官方模块 | 注册方式 | Test Flag | 目的 |
|---|---|---|---|---|
| **L1** | Simple Automation Test | `IMPLEMENT_SIMPLE_AUTOMATION_TEST` | EditorContext + ProductFilter | 单接口正确性 |
| **L2** | Automation Spec (BDD) | `BEGIN_DEFINE_SPEC` | EditorContext + ProductFilter | 多接口闭环（写→读→验） |
| **L3** | Functional Testing | AFunctionalTest 子类 | EditorContext + ProductFilter | 完整 Demo 场景 |

### Session Frontend 树

```
Project.AgentBridge
├── L1.Query
│   ├── GetCurrentProjectState     ← T1-01
│   ├── ListLevelActors            ← T1-02
│   ├── GetActorState              ← T1-03
│   ├── GetActorBounds             ← T1-04
│   ├── GetAssetMetadata           ← T1-05
│   ├── GetDirtyAssets             ← T1-06
│   └── RunMapCheck                ← T1-07
├── L1.Write
│   ├── SpawnActor                 ← T1-08
│   ├── SetActorTransform          ← T1-09
│   ├── ImportAssets               ← T1-10
│   └── CreateBlueprintChild       ← T1-11
├── L3.UITool                      ← L3 UI 工具单接口测试分组
│   ├── IsAutomationDriverAvailable ← T1-12
│   ├── ClickDetailPanelButton     ← T1-13
│   ├── TypeInDetailPanelField     ← T1-14
│   └── DragAssetToViewport        ← T1-15（含 L3→L1 交叉比对）
├── L2
│   ├── SpawnReadbackLoop          ← LT-01
│   ├── TransformModifyLoop        ← LT-02
│   └── ImportMetadataLoop         ← LT-03
├── L2.UITool                      ← L3 UI 工具的 L2 闭环验证
│   ├── DragAssetToViewportLoop    ← LT-04（5 It，含交叉比对）
│   └── TypeInFieldLoop            ← LT-05（3 It）
└── L3
    └── FunctionalTest (FTEST_WarehouseDemo 地图)
```

---

## 3. 前置条件

| 条件 | 要求 |
|---|---|
| UE5.5.4 Editor | 已启动并加载项目 |
| AgentBridge Plugin | 已启用（全部 Bridge 接口可用） |
| AgentBridgeTests Plugin | 已启用（测试注册到 Session Frontend） |
| Remote Control API | Plugin 已启用，HTTP Server 已启动（通道 B/C 测试需要） |
| 测试关卡 | `/Game/Maps/TestMap` 已加载（L1/L2）或 `FTEST_WarehouseDemo`（L3） |
| Schema 校验链 | `validate_examples.py --strict` 返回 0 |

---

## 4. L1：单接口测试

### 4.1 实现位置

```
Plugins/AgentBridgeTests/Source/AgentBridgeTests/Private/
├── L1_QueryTests.cpp    ← 7 个查询接口
├── L1_WriteTests.cpp    ← 4 个写接口
└── L1_UIToolTests.cpp   ← 4 个 L3 UI 工具接口（含 L3→L1 交叉比对）
```

### 4.2 L1 测试清单

| ID | 测试名 | 覆盖场景 | 关键断言 |
|---|---|---|---|
| T1-01 | GetCurrentProjectState | happy path | data 含 project_name / engine_version / editor_mode |
| T1-02 | ListLevelActors | happy + ClassFilter | actors 数组含 actor_name / actor_path / class |
| T1-03 | GetActorState | happy + 404 + 空参数 | transform/collision/tags 结构 + ACTOR_NOT_FOUND + validation_error |
| T1-04 | GetActorBounds | happy path | origin/extent 为 3 元素数组 |
| T1-05 | GetAssetMetadata | 不存在资产 + 空参数 | exists=false + validation_error |
| T1-06 | GetDirtyAssets | happy path | dirty_assets 数组存在 |
| T1-07 | RunMapCheck | happy path | map_errors / map_warnings 字段存在 |
| T1-08 | SpawnActor | 校验×3 + dry_run + 执行 + Transaction + 读回容差 | TestNearlyEqual(location, 0.01) + bTransaction=true |
| T1-09 | SetActorTransform | 校验 + dry_run + 执行 + old/actual + Undo | old_transform 匹配原值 + Undo 后恢复 |
| T1-10 | ImportAssets | 校验×2 + dry_run | dry_run 返回 success（无测试资源时不做实际导入） |
| T1-11 | CreateBlueprintChild | 校验×2 + dry_run + 不存在父类 + 实际创建 | CLASS_NOT_FOUND + created_objects |
| T1-12 | IsAutomationDriverAvailable | 可用性查询 + Adapter 一致性 | bool 返回值与 FAutomationDriverAdapter 一致 |
| T1-13 | ClickDetailPanelButton | 参数校验×2 + dry_run + Actor 不存在 + Driver 不可用 | validation_error + ACTOR_NOT_FOUND + DRIVER_NOT_AVAILABLE |
| T1-14 | TypeInDetailPanelField | 参数校验×3 + dry_run | validation_error + tool_layer=L3_UITool |
| T1-15 | DragAssetToViewport | 参数校验 + dry_run + 实际执行 + **L3→L1 交叉比对** | actors_created + CrossVerifyUIOperation consistent=true |

### 4.3 L1 设计模式

每个 L1 测试遵循统一模式：

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBridgeL1_Xxx,
    "Project.AgentBridge.L1.Category.Xxx",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_Xxx::RunTest(const FString& Parameters)
{
    // 1. 获取 Subsystem
    UAgentBridgeSubsystem* Sub = GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>();

    // 2. 调用接口（通道 C：C++ 直接调用）
    FBridgeResponse Response = Sub->XxxFunction(params);

    // 3. 断言 status
    TestEqual(TEXT("status"), BridgeStatusToString(Response.Status), TEXT("success"));

    // 4. 断言 data 结构
    TestTrue(TEXT("has field"), Response.Data->HasField(TEXT("xxx")));

    // 5. 写测试额外：断言 Transaction + Undo 清理
    TestTrue(TEXT("transaction"), Response.bTransaction);
    GEditor->UndoTransaction();

    return true;
}
```

---

## 5. L2：闭环验证

### 5.1 实现位置

```
Plugins/AgentBridgeTests/Source/AgentBridgeTests/Private/
├── L2_ClosedLoopSpecs.spec.cpp         ← 3 个语义工具 BDD Spec
└── L2_UIToolClosedLoopSpec.spec.cpp    ← 2 个 L3 UI 工具闭环 Spec
```

### 5.2 L2 测试清单

| ID | Spec 名 | Describe | It 数量 | 闭环模式 |
|---|---|---|---|---|
| LT-01 | SpawnReadbackLoop | spawn → readback | 5 | SpawnActor → GetActorState（容差验证 location/rotation/scale）+ GetActorBounds + GetDirtyAssets |
| LT-02 | TransformModifyLoop | modify → readback | 3 | SetActorTransform → 验证 old_transform + readback 新值 + **Undo 回滚验证** |
| LT-03 | ImportMetadataLoop | import → metadata | 2 | ImportAssets → GetAssetMetadata(exists=true) + GetDirtyAssets |
| LT-04 | DragAssetToViewportLoop | L3 drag → L1 verify | 5 | L3 DragAssetToViewport → L1 ListLevelActors（Actor 数增加）→ **L3→L1 交叉比对**（consistent）→ L1 GetActorState（位置容差 100cm）→ Undo 回滚 |
| LT-05 | TypeInFieldLoop | L3 type → L1 verify | 3 | L1 SpawnActor 准备 → L1 读回基线 → L3 TypeInDetailPanelField → L3→L1 交叉比对 → **L1 读回在 L3 操作后仍可用** |

### 5.3 L2 BDD 语法

```cpp
BEGIN_DEFINE_SPEC(FBridgeL2_SpawnReadbackLoop,
    "Project.AgentBridge.L2.SpawnReadbackLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
    // 共享状态
    FBridgeTransform InputTransform;
    FString SpawnedActorPath;
END_DEFINE_SPEC(FBridgeL2_SpawnReadbackLoop)

void FBridgeL2_SpawnReadbackLoop::Define()
{
    Describe("spawn actor then readback", [this]()
    {
        BeforeEach([this]() { /* Spawn 测试 Actor */ });

        It("should return matching location on readback", [this]()
        {
            // GetActorState → TestNearlyEqual(location, 0.01f)
        });

        It("should return matching rotation on readback", [this]() { ... });
        It("should return matching scale on readback", [this]() { ... });
        It("should be visible in GetActorBounds", [this]() { ... });
        It("should mark level as dirty", [this]() { ... });

        AfterEach([this]() { GEditor->UndoTransaction(); });
    });
}
```

### 5.4 L2 vs L1 的关键区别

| 维度 | L1 | L2 |
|---|---|---|
| 粒度 | 单接口 | **多接口协作** |
| Test Flag | ProductFilter | **ProductFilter（UE5.5 控制台稳定路径）** |
| 语法 | Simple Test | **Automation Spec (BDD)** |
| 核心 | 返回值正确 | **写后读回一致性 + Undo 回滚** |
| 生命周期 | RunTest 一次性 | **BeforeEach / AfterEach 自动管理** |

### 5.5 容差标准

**L1 语义工具容差**（API 直接设值，精度极高）：

| 字段 | 容差 | UE5 断言 |
|---|---|---|
| location (cm) | ≤ 0.01 | `TestNearlyEqual(Actual, Expected, 0.01f)` |
| rotation (degrees) | ≤ 0.01 | `TestNearlyEqual(Actual, Expected, 0.01f)` |
| scale (倍率) | ≤ 0.001 | `TestNearlyEqual(Actual, Expected, 0.001f)` |

**L3 UI 工具容差**（拖拽操作依赖 Viewport 投影精度和鼠标释放位置）：

| 字段 | 容差 | 原因 |
|---|---|---|
| location (cm) | ≤ 100 | Viewport 投影 + 鼠标释放偏差 + 自动贴地 |

这些容差替代了 v0.2 中 `verifier.py` 的自建比对逻辑。L3 的宽容差是 UI 操作固有精度限制——这也是 L1 语义工具是默认主干的原因之一。

---

## 6. L3：完整 Demo 验证

### 6.1 实现位置

```
Plugins/AgentBridgeTests/Source/AgentBridgeTests/Private/
├── L3_FunctionalTestActor.h
└── L3_FunctionalTestActor.cpp

Content/Tests/
└── FTEST_WarehouseDemo.umap     ← 测试地图（需手工创建）
```

### 6.2 L3 工作流

```
1. 创建 FTEST_WarehouseDemo 测试地图
2. 在地图中放置 AAgentBridgeFunctionalTest Actor
3. 设置属性：
   - SpecPath = "AgentSpecs/levels/warehouse_layout.yaml"（或留空用内置场景）
   - LocationTolerance = 0.01
   - BuiltInActorCount = 5
   - bUndoAfterTest = true
4. Session Frontend → Run Level Test → 自动识别 FTEST_ 地图
5. 执行流程：
   GetCurrentProjectState → 循环 N 次 SpawnActor+验证 → MapCheck → DirtyAssets → NonOverlap
6. FinishTest(Succeeded / Failed)
7. CleanUp → Undo × N
```

### 6.3 两种模式

| 模式 | SpecPath | 行为 |
|---|---|---|
| 内置场景 | 空 | 自动生成 N 个 Actor（均匀分布位置/旋转/缩放），逐个验证 |
| Spec 驱动 | 指向 YAML | 通过 Python Orchestrator 执行 Spec，再验证结果 |

---

## 7. 执行方式总览

| 执行方式 | 命令 | 覆盖层 | 适用场景 |
|---|---|---|---|
| **Session Frontend UI** | Editor → Window → Session Frontend → 勾选测试 → Run | L1/L2/L3 | 开发中手工验证 |
| **Console Command（两段式）** | `Automation RunTests Project.AgentBridge.L1` → `Automation RunTests Project.AgentBridge.L2` | L1/L2 | Editor 内快速执行（UE5.5 推荐） |
| **Commandlet** | `UE5Editor-Cmd -run=AgentBridge -RunTests="..."` | L1/L2 | 无头执行 |
| **UAT** | `RunUAT BuildCookRun -run -editortest -RunAutomationTest=Project.AgentBridge.L2 ...` | L1/L2 | CI/CD |
| **Gauntlet** | `RunUAT RunGauntlet -Test=AgentBridge.AllTests` | L1/L2/L3 | 完整 CI/CD 流水线 |
| **Schema 校验** | `python validate_examples.py --strict` | Schema 层 | 任何时候 |

### 7.1 Gauntlet CI/CD 配置

| 配置 | 命令 | 超时 | 用途 |
|---|---|---|---|
| AllTests | `RunUAT RunGauntlet -Test=AgentBridge.AllTests` | 600s | 每日构建 |
| SmokeTests | `RunUAT RunGauntlet -Test=AgentBridge.SmokeTests` | 180s | 每次提交 |
| SpecExecution | `RunUAT RunGauntlet -Test=AgentBridge.SpecExecution -SpecPath=xxx` | 300s | 验证特定 Spec |

### 7.2 UE5.5 两段式控制台标准流程

在 UE5.5 命令行/控制台路径中，L1 与 L2 采用固定顺序执行，避免聚合运行带来的不稳定因素：

1. Phase 1：`Automation RunTests Project.AgentBridge.L1`
2. Phase 2：`Automation RunTests Project.AgentBridge.L2`

说明：
- 不再使用 `Automation RunTests Project.AgentBridge` 作为验收口径。
- VS Code 任务统一使用 `Automation Two-Phase (L1 then L2)` 作为默认测试入口。

---

## 8. 测试记录模板

```yaml
test_record:
  date: "2026-03-20"
  engine_version: "5.5.4"
  test_level: /Game/Maps/TestMap
  plugin_version: "0.3.0"
  execution_method: "session_frontend"  # session_frontend / console / commandlet / uat / gauntlet

  l1_results:
    T1-01_GetCurrentProjectState: PASS
    T1-02_ListLevelActors: PASS
    T1-03_GetActorState: PASS
    T1-04_GetActorBounds: PASS
    T1-05_GetAssetMetadata: PASS
    T1-06_GetDirtyAssets: PASS
    T1-07_RunMapCheck: PASS
    T1-08_SpawnActor: PASS
    T1-09_SetActorTransform: PASS
    T1-10_ImportAssets: PASS
    T1-11_CreateBlueprintChild: PASS
    T1-12_IsAutomationDriverAvailable: PASS
    T1-13_ClickDetailPanelButton: PASS  # 或 SKIP（Driver 不可用时）
    T1-14_TypeInDetailPanelField: PASS  # 或 SKIP
    T1-15_DragAssetToViewport: PASS     # 含 L3→L1 交叉比对

  l2_results:
    LT-01_SpawnReadbackLoop: PASS
    LT-02_TransformModifyLoop: PASS
    LT-03_ImportMetadataLoop: SKIP  # 无测试资源时
    LT-04_DragAssetToViewportLoop: PASS  # L3→L1 交叉比对 consistent=true
    LT-05_TypeInFieldLoop: PASS          # L3 操作后 L1 读回仍可用

  l2_extras:
    undo_after_spawn: PASS
    undo_after_transform: PASS
    tolerance_location_L1: 0.01
    tolerance_rotation_L1: 0.01
    tolerance_scale_L1: 0.001
    tolerance_location_L3: 100.0   # UI 操作精度

  l3_results:
    FTEST_WarehouseDemo: PASS

  schema_validation:
    validate_examples_strict: PASS  # 8/8

  notes: ""
```

### 8.1 Task19 最终测试记录（2026-03-26）

```yaml
test_record:
  date: "2026-03-26"
  engine_version: "5.5.4"
  test_level: /Game/Tests/FTEST_WarehouseDemo
  plugin_version: "0.3.0"
  execution_method: "mixed"  # console / commandlet / gauntlet / execute_python_script

  l1_results:
    T1-01_GetCurrentProjectState: PASS
    T1-02_ListLevelActors: PASS
    T1-03_GetActorState: PASS
    T1-04_GetActorBounds: PASS
    T1-05_GetAssetMetadata: PASS
    T1-06_GetDirtyAssets: PASS
    T1-07_RunMapCheck: PASS
    T1-08_SpawnActor: PASS
    T1-09_SetActorTransform: PASS
    T1-10_ImportAssets: PASS
    T1-11_CreateBlueprintChild: PASS
    T1-12_IsAutomationDriverAvailable: PASS
    T1-13_ClickDetailPanelButton: PASS
    T1-14_TypeInDetailPanelField: PASS
    T1-15_DragAssetToViewport: PASS

  l2_results:
    LT-01_SpawnReadbackLoop: PASS
    LT-02_TransformModifyLoop: PASS
    LT-03_ImportMetadataLoop: SKIP
    LT-04_DragAssetToViewportLoop: PASS
    LT-05_TypeInFieldLoop: PASS

  l2_extras:
    undo_after_spawn: PASS
    undo_after_transform: PASS
    tolerance_location_L1: 0.01
    tolerance_rotation_L1: 0.01
    tolerance_scale_L1: 0.001
    tolerance_location_L3: 100.0

  l3_results:
    FTEST_WarehouseDemo: PASS

  schema_validation:
    validate_examples_strict: PASS  # 10/10

  notes: "Task19 Step4 最终使用已验证 runtime spec；Task19 Step7 已修复通道 B 直接读取 Actor Relative* 属性在 UE5.5.4 下返回 400 的问题，三通道一致性恢复。"
```

---

## 9. 与路线图的对应关系

| 阶段 | 应执行的测试 | 执行方式 |
|---|---|---|
| Plugin 核心开发（第 2-3 周） | L1 Query + Write（11 个）| Session Frontend |
| 测试 Plugin 开发（第 4-5 周） | L1（11 个）+ L3.UITool（4 个）+ L2（5 个） | Session Frontend + Console |
| 完整集成（第 6-8 周） | L1 + L2 + L3 | Commandlet + UAT |
| CI/CD 上线 | 全部 | Gauntlet |
| 任何代码变更后 | 至少 L1 全部 | 最方便的方式 |

---

## 10. 与其他文档的关系

| 文档 | 关系 |
|---|---|
| `architecture_overview.md` | 验证层架构全图 |
| `bridge_implementation_plan.md` | Bridge 接口实现（被测试的目标） |
| `bridge_verification_and_error_handling.md` | 错误处理和验证细节 |
| `ue5_capability_map.md` | UE5 测试模块详细说明 |
| `tool_contract_v0_1.md` | 工具契约（测试验收标准的来源） |

