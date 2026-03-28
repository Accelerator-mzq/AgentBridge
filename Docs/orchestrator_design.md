# Orchestrator 设计方案

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 文档目的

本文档定义 Orchestrator（Agent 编排层）的实现方案：如何读取结构化 Spec、选择工具、组织"查询 → 执行 → 读回 → 验证 → 报告"闭环。

本文档是第 4 周的实现参考。第 2-3 周应先完成 Bridge 函数和手工闭环验证，第 4 周再将手工流程固化为 Orchestrator 自动编排。

---

## 2. 定位与边界

### 2.1 Orchestrator 是什么

Orchestrator 是连接"结构化 Spec"和"Bridge 工具函数"的编排层。它负责：

- 读取 Spec 文件
- 解析 Actor 列表和约束
- 针对每个 Actor 决定操作类型（创建 / 修改 / 跳过）
- 调用 Bridge 函数执行操作
- 组织写后读回和验证
- 汇总生成执行报告

### 2.2 Orchestrator 不是什么

- 不是 AI Agent 本身（Agent 调用 Orchestrator，Orchestrator 不做"理解需求"的工作）
- 不是 Spec 生成器（Spec 由 Agent 或人工提前生成）
- 不是分布式调度系统（当前为单进程 Python 脚本）
- 不做 UE5 API 调用（那是 Bridge 的职责）

### 2.2.5 与 UE5 Gauntlet 的对照关系

UE5 官方提供了 **Gauntlet** 框架——一个外部测试会话编排框架（C#，基于 UAT）。Orchestrator 与 Gauntlet 在结构上高度同构：

| 维度 | 我们的 Orchestrator | UE5 Gauntlet |
|---|---|---|
| 运行位置 | 引擎外部（Python） | 引擎外部（C#，基于 UAT） |
| 职责 | 编排开发操作 | 编排测试会话 |
| 引擎内触角 | Bridge 函数（Python / HTTP） | GauntletTestController（C++，OnTick 驱动） |
| 结果收集 | JSON 返回值 + 结构化报告 | 进程退出码 + 日志解析 + Saved 文件 |
| 会话配置 | Spec YAML（actors / constraints） | C# TestConfig（Roles / Controllers） |
| 超时机制 | 通过 safe_execute(timeout) 参数实现 | 内置 MaxDuration |
| 进程管理 | 当前假设 Editor 已手工启动（Gauntlet 可自动启动） | 自动启动/监控/停止 UE 进程 |
| 多实例支持 | 当前不支持 | 原生支持（N Client + 1 Server） |

**两者不是替代关系**——Gauntlet 编排"测试会话"，Orchestrator 编排"开发操作"。v0.3 中 Gauntlet 已实际集成（C# TestConfig + GauntletTestController）：

1. **超时机制**：每个工具调用通过 `safe_execute(timeout=N)` 设置超时，超时返回 TIMEOUT 错误码
2. **异步等待**：对资产导入等长操作使用轮询等待（poll until ready）
3. **结果收集多手段**：JSON 返回值 + UE5 Automation Report 格式双输出
4. **进程管理**：v0.3 已通过 Gauntlet C# 端实现自动启动/监控/超时/停止 Editor 进程
5. **崩溃恢复**：Gauntlet C# 端 MaxDuration 超时后自动终止挂起进程

**v0.3 Gauntlet CI/CD 流程**：
```
RunUAT RunGauntlet -Test=AgentBridge.AllTests -project=MyGame.uproject
  → Gauntlet C# 启动 Editor（-Unattended -NullRHI）
  → AgentBridgeGauntletController.OnInit() 解析参数
  → OnTick() 触发 Automation RunTests + 轮询完成
  → 收集结果 → EndTest(ExitCode)
  → Gauntlet C# 判定通过/失败 → 停止 Editor
```

> 完整的 Gauntlet 能力说明参见 `Docs/ue5_capability_map.md` 第 4.3.5 节。

### 2.3 与其他模块的关系

```
Agent（规划/决策）
  ↓ 提供结构化 Spec
Orchestrator（编排）
  ↓ 调用 Bridge 函数（通过通道 A 或通道 B）
Bridge 封装层（参数校验 + 统一响应 + UE5 API 调用）
  ├── 通道 A: Python Editor Scripting（进程内）
  └── 通道 B: Remote Control API（HTTP 远程）
  ↓
UE5.5.4 Editor
```

Orchestrator 依赖：
- `Specs/templates/scene_spec_template.yaml`（Spec 格式定义）
- `Scripts/bridge/query_tools.py`（反馈接口，三通道）
- `Scripts/bridge/write_tools.py`（写接口，三通道 + FScopedTransaction）
- `Docs/feedback_write_mapping.md`（闭环映射）

---

## 3. 代码结构

### 3.1 目录布局

```
Scripts/
├── bridge/                  # Bridge 层（已有）
│   ├── bridge_core.py
│   ├── query_tools.py
│   ├── write_tools.py
│   ├── validation_tools.py
│   └── ue_helpers.py
└── orchestrator/            # 编排层（新增）
    ├── __init__.py
    ├── orchestrator.py      # 主编排逻辑
    ├── spec_reader.py       # Spec 文件解析
    ├── plan_generator.py    # 执行计划生成
    ├── verifier.py          # 验证逻辑（actual vs expected）
    └── report_generator.py  # 报告生成
```

### 3.2 模块职责

| 模块 | 职责 | 不做 |
|---|---|---|
| `orchestrator.py` | 主循环：读 Spec → 生成计划 → 逐步执行 → 汇总报告 | 不做 UE API 调用 |
| `spec_reader.py` | 解析 YAML Spec 文件，输出结构化的 Actor 列表和约束 | 不做 Spec 生成 |
| `plan_generator.py` | 对比 Spec 与当前状态，决定每个 Actor 的操作类型 | 不做执行 |
| `verifier.py` | 将读回的 actual 值与 Spec 中的 expected 值比对，输出判定 | 不做业务规划 |
| `report_generator.py` | 汇总执行过程生成结构化报告 | 不做执行或验证 |

---

## 4. 核心流程

### 4.1 总体流程（对应 AGENTS.md 第 6 节 10 步）

```
Step 1: 读取 Spec
  spec_reader.py 解析 YAML → 输出 actor_list / constraints / defaults / validation_rules

Step 2: 查询当前状态
  调用 get_current_project_state() → 确认项目/关卡正确
  调用 list_level_actors() → 获取当前 Actor 列表

Step 3: 生成执行计划
  plan_generator.py 对比 Spec 中的 Actor 与当前 Actor 列表
  → 决定每个 Actor：CREATE / UPDATE / SKIP
  → 识别 execution_method 字段：semantic（默认）/ ui_tool（L3）

Step 4: 逐个执行（按工具层次分发）
  对每个计划条目：
    如果 execution_method == semantic（默认）：
      CREATE → 调用 spawn_actor()（L1 语义工具）
      UPDATE → 调用 set_actor_transform()（L1 语义工具）
    如果 execution_method == ui_tool：
      根据 ui_action.type 分发到 L3 UI 工具：
        drag_asset_to_viewport → ui_tools.drag_asset_to_viewport()
        click_detail_panel_button → ui_tools.click_detail_panel_button()
        type_in_detail_panel_field → ui_tools.type_in_detail_panel_field()
    记录写接口返回的 actual_*（第一次读回）

Step 5: 二次确认
  对每个已执行的 Actor：
    调用 get_actor_state() → 获取二次读回
    调用 get_actor_bounds() → 获取占地
  L3 UI 工具额外步骤：
    执行 L3→L1 交叉比对（cross_verify_ui_operation）
    对比 L3 返回值与 L1 独立读回——两者一致 → success，不一致 → mismatch

Step 6: 验证
  verifier.py 将二次读回的 actual 与 Spec 中的 expected 逐字段比对
  → 输出每个 Actor 的验证结果（success / mismatch）

Step 7: 副作用检查
  调用 get_dirty_assets() → 记录脏资产
  调用 run_map_check() → 记录 map errors/warnings

Step 8: 生成报告
  report_generator.py 汇总全部信息 → 输出结构化报告
```

### 4.2 流程图

```
           ┌──────────────┐
           │  读取 Spec    │
           └──────┬───────┘
                  ▼
           ┌──────────────┐
           │ 查询当前状态   │ ← get_current_project_state + list_level_actors
           └──────┬───────┘
                  ▼
           ┌──────────────┐
           │ 生成执行计划   │ → 每个 Actor: CREATE / UPDATE / SKIP
           └──────┬───────┘
                  ▼
           ┌──────────────┐
      ┌───►│ 执行下一条目   │ ← spawn_actor / set_actor_transform
      │    └──────┬───────┘
      │           ▼
      │    ┌──────────────┐
      │    │ 第一次读回     │ ← 写接口自身返回 actual_*
      │    └──────┬───────┘
      │           ▼
      │    ┌──────────────┐
      │    │ 二次确认      │ ← get_actor_state + get_actor_bounds
      │    └──────┬───────┘
      │           ▼
      │    ┌──────────────┐
      │    │ 验证          │ ← actual vs expected（含容差）
      │    └──────┬───────┘
      │           ▼
      │    ┌──────────────┐
      │    │ 还有条目？    │──是──┘
      │    └──────┬───────┘
      │           │ 否
      │           ▼
      │    ┌──────────────┐
      │    │ 副作用检查    │ ← get_dirty_assets + run_map_check
      │    └──────┬───────┘
      │           ▼
      │    ┌──────────────┐
      └────│ 生成报告      │
           └──────────────┘
```

---

## 5. 模块实现骨架

### 5.1 spec_reader.py

```python
"""解析结构化 Spec 文件。"""

import yaml
from pathlib import Path
from typing import Dict, List, Any

def read_spec(spec_path: str) -> Dict[str, Any]:
    """
    读取 YAML Spec 文件，返回结构化数据。

    返回格式：
    {
        "scene": { "scene_id": ..., "target_level": ... },
        "defaults": { "size_profile_to_scale": ... },
        "layout": { "playable_area": ... },
        "anchors": { ... },
        "actors": [ { "id": ..., "class": ..., "transform": ..., ... } ],
        "validation": { "rules": [ ... ] },
    }
    """
    path = Path(spec_path)
    with path.open("r", encoding="utf-8") as f:
        spec = yaml.safe_load(f)

    # 基础校验
    assert "scene" in spec, "Spec 缺少 scene 字段"
    assert "actors" in spec, "Spec 缺少 actors 字段"

    for actor in spec["actors"]:
        assert "id" in actor, f"Actor 缺少 id 字段"
        assert "class" in actor, f"Actor {actor.get('id', '?')} 缺少 class 字段"
        assert "transform" in actor, f"Actor {actor['id']} 缺少 transform 字段"
        t = actor["transform"]
        assert "location" in t, f"Actor {actor['id']} transform 缺少 location"
        assert "rotation" in t, f"Actor {actor['id']} transform 缺少 rotation"
        assert "relative_scale3d" in t, f"Actor {actor['id']} transform 缺少 relative_scale3d"

    return spec
```

### 5.2 plan_generator.py

```python
"""对比 Spec 与当前状态，生成执行计划。"""

from typing import Dict, List, Any

# 操作类型
ACTION_CREATE = "CREATE"
ACTION_UPDATE = "UPDATE"
ACTION_SKIP = "SKIP"
ACTION_UI_TOOL = "UI_TOOL"   # L3 UI 工具操作

def generate_plan(
    spec_actors: List[Dict[str, Any]],
    existing_actors: List[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    """
    对比 Spec 中的 Actor 列表与当前关卡中的 Actor 列表，
    为每个 Spec Actor 决定操作类型。

    execution_method 字段决定工具层次：
      "semantic"（默认）→ L1 语义工具（spawn_actor / set_actor_transform）
      "ui_tool" → L3 UI 工具（drag_asset_to_viewport / click_detail_panel_button 等）

    返回：
    [
        {
            "actor_spec": { ... },
            "action": "CREATE" | "UPDATE" | "UI_TOOL" | "SKIP",
            "execution_method": "semantic" | "ui_tool",
            "existing_actor_path": None,
            "reason": "...",
        },
        ...
    ]
    """
    existing_by_name = {}
    for actor in existing_actors:
        existing_by_name[actor.get("actor_name", "")] = actor

    plan = []
    for spec_actor in spec_actors:
        actor_id = spec_actor["id"]
        exec_method = spec_actor.get("execution_method", "semantic")

        # L3 UI 工具操作——不走 CREATE/UPDATE 判定
        if exec_method == "ui_tool":
            plan.append({
                "actor_spec": spec_actor,
                "action": ACTION_UI_TOOL,
                "execution_method": "ui_tool",
                "existing_actor_path": None,
                "reason": f"Actor '{actor_id}' uses L3 UI tool: {spec_actor.get('ui_action', {}).get('type', '?')}",
            })
            continue

        # L1 语义工具——标准 CREATE/UPDATE 判定
        if actor_id in existing_by_name:
            plan.append({
                "actor_spec": spec_actor,
                "action": ACTION_UPDATE,
                "execution_method": "semantic",
                "existing_actor_path": existing_by_name[actor_id]["actor_path"],
                "reason": f"Actor '{actor_id}' already exists, will update transform",
            })
        else:
            plan.append({
                "actor_spec": spec_actor,
                "action": ACTION_CREATE,
                "execution_method": "semantic",
                "existing_actor_path": None,
                "reason": f"Actor '{actor_id}' not found, will create",
            })

    return plan
```

### 5.3 verifier.py

```python
"""将读回的 actual 值与 Spec expected 值比对。"""

from typing import Dict, Any, List
import math

# 默认容差（与 AGENTS.md 第 7.2 节一致）
DEFAULT_TOLERANCES = {
    "location": 0.01,           # cm
    "rotation": 0.01,           # degrees
    "relative_scale3d": 0.001,  # 倍率
    "world_bounds_extent": 1.0, # cm
}

def verify_transform(
    expected: Dict[str, list],
    actual: Dict[str, list],
    tolerances: Dict[str, float] = None,
) -> Dict[str, Any]:
    """
    逐字段比对 transform，返回结构化验证结果。

    返回：
    {
        "status": "success" | "mismatch",
        "checks": [
            { "field": "location", "expected": [...], "actual": [...],
              "tolerance": 0.01, "pass": True },
            ...
        ]
    }
    """
    tol = tolerances or DEFAULT_TOLERANCES
    checks = []
    all_pass = True

    for field in ["location", "rotation", "relative_scale3d"]:
        exp = expected.get(field, [0, 0, 0])
        act = actual.get(field, [0, 0, 0])
        field_tol = tol.get(field, 0.01)

        field_pass = all(
            abs(a - e) <= field_tol
            for a, e in zip(act, exp)
        )

        checks.append({
            "field": field,
            "expected": exp,
            "actual": act,
            "tolerance": field_tol,
            "pass": field_pass,
        })

        if not field_pass:
            all_pass = False

    return {
        "status": "success" if all_pass else "mismatch",
        "checks": checks,
    }
```

### 5.4 report_generator.py

```python
"""汇总执行过程生成结构化报告。"""

import json
from typing import Dict, List, Any
from datetime import datetime

def generate_report(
    spec_path: str,
    plan: List[Dict[str, Any]],
    execution_results: List[Dict[str, Any]],
    verification_results: List[Dict[str, Any]],
    dirty_assets: List[str],
    map_check: Dict[str, Any],
) -> Dict[str, Any]:
    """
    生成结构化执行报告。

    报告包含：
    - 执行元数据（时间、Spec、关卡）
    - 执行计划
    - 逐 Actor 的执行结果和验证结果
    - 副作用（dirty assets / map check）
    - 总结状态
    """
    all_success = all(
        v.get("status") == "success"
        for v in verification_results
    )

    return {
        "report_version": "v0.1",
        "timestamp": datetime.now().isoformat(),
        "spec_path": spec_path,
        "overall_status": "success" if all_success else "mismatch",
        "plan": [
            {
                "actor_id": item["actor_spec"]["id"],
                "action": item["action"],
                "reason": item["reason"],
            }
            for item in plan
        ],
        "results": [
            {
                "actor_id": er.get("actor_id", ""),
                "execution_status": er.get("status", ""),
                "verification": vr,
            }
            for er, vr in zip(execution_results, verification_results)
        ],
        "side_effects": {
            "dirty_assets": dirty_assets,
            "map_errors": map_check.get("data", {}).get("map_errors", []),
            "map_warnings": map_check.get("data", {}).get("map_warnings", []),
        },
    }
```

### 5.5 orchestrator.py（主编排）

```python
"""Orchestrator 主编排逻辑。"""

from spec_reader import read_spec
from plan_generator import generate_plan, ACTION_CREATE, ACTION_UPDATE, ACTION_UI_TOOL
from verifier import verify_transform
from report_generator import generate_report

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
from query_tools import (
    get_current_project_state,
    list_level_actors,
    get_actor_state,
    get_actor_bounds,
    get_dirty_assets,
)
from write_tools import spawn_actor, set_actor_transform
from validation_tools import run_map_check
from ui_tools import (
    click_detail_panel_button,
    type_in_detail_panel_field,
    drag_asset_to_viewport,
    cross_verify_ui_operation,
)

# L3 UI 工具类型 → 调用函数映射
_UI_TOOL_DISPATCH = {
    "drag_asset_to_viewport": drag_asset_to_viewport,
    "click_detail_panel_button": click_detail_panel_button,
    "type_in_detail_panel_field": type_in_detail_panel_field,
}

# L3 UI 工具类型 → L1 验证函数 + 参数构造
_UI_TOOL_VERIFY = {
    "drag_asset_to_viewport": {
        "l1_func": list_level_actors,
        "l1_args": None,  # 无需参数
    },
    "click_detail_panel_button": {
        "l1_func": get_actor_state,
        "l1_args_key": "actor_path",  # 从 ui_action 中取 actor_path
    },
    "type_in_detail_panel_field": {
        "l1_func": get_actor_state,
        "l1_args_key": "actor_path",
    },
}

def run(spec_path: str) -> dict:
    """
    主编排入口。读取 Spec → 执行 → 验证 → 报告。
    支持 L1 语义工具（默认主干）和 L3 UI 工具（execution_method: ui_tool）。
    """
    # Step 1: 读取 Spec
    spec = read_spec(spec_path)
    target_level = spec["scene"]["target_level"]

    # Step 2: 查询当前状态
    project_state = get_current_project_state()
    if project_state["status"] != "success":
        return project_state

    current_actors_response = list_level_actors()
    current_actors = current_actors_response.get("data", {}).get("actors", [])

    # Step 3: 生成执行计划（含 execution_method 分发）
    plan = generate_plan(spec["actors"], current_actors)

    # Step 4-6: 逐个执行 + 读回 + 验证
    execution_results = []
    verification_results = []

    for item in plan:
        actor_spec = item["actor_spec"]
        action = item["action"]
        exec_method = item.get("execution_method", "semantic")

        # ========================================
        # L1 语义工具（默认主干）
        # ========================================
        if action == ACTION_CREATE:
            result = spawn_actor(
                level_path=target_level,
                actor_class=actor_spec["class"],
                actor_name=actor_spec["id"],
                transform=actor_spec["transform"],
            )
            actor_path = None
            if result["status"] == "success":
                created = result["data"].get("created_objects", [])
                if created:
                    actor_path = created[0].get("actor_path")

        elif action == ACTION_UPDATE:
            actor_path = item["existing_actor_path"]
            result = set_actor_transform(
                actor_path=actor_path,
                transform=actor_spec["transform"],
            )

        # ========================================
        # L3 UI 工具（仅当 execution_method == ui_tool）
        # ========================================
        elif action == ACTION_UI_TOOL:
            ui_action = actor_spec.get("ui_action", {})
            ui_type = ui_action.get("type", "")
            dispatch_fn = _UI_TOOL_DISPATCH.get(ui_type)

            if not dispatch_fn:
                result = {"status": "failed", "summary": f"Unknown ui_action type: {ui_type}"}
                actor_path = None
            else:
                # 构造 L3 调用参数（从 ui_action 中提取）
                l3_kwargs = {k: v for k, v in ui_action.items() if k != "type"}
                result = dispatch_fn(**l3_kwargs)
                actor_path = ui_action.get("actor_path")

            # L3→L1 交叉比对
            if result.get("status") == "success":
                verify_config = _UI_TOOL_VERIFY.get(ui_type, {})
                l1_func = verify_config.get("l1_func")
                l1_args = None
                if "l1_args_key" in verify_config and ui_action.get(verify_config["l1_args_key"]):
                    l1_args = {verify_config["l1_args_key"]: ui_action[verify_config["l1_args_key"]]}

                cross_result = cross_verify_ui_operation(
                    l3_response=result,
                    l1_verify_func=l1_func,
                    l1_verify_args=l1_args,
                )
                # 用交叉比对的 final_status 覆盖 result status
                result["cross_verification"] = cross_result
                if cross_result.get("final_status") == "mismatch":
                    result["status"] = "mismatch"
                    result["summary"] += f" (L3/L1 mismatch: {cross_result.get('mismatches', [])})"
        else:
            continue

        # 记录执行结果
        exec_record = {
            "actor_id": actor_spec["id"],
            "action": action,
            "execution_method": exec_method,
            "status": result["status"],
            "actor_path": actor_path,
            "first_readback": result.get("data", {}).get("actual_transform"),
        }
        execution_results.append(exec_record)

        # L1 语义工具的二次确认 + 验证
        if exec_method == "semantic" and result["status"] == "success" and actor_path:
            state = get_actor_state(actor_path=actor_path)
            actual_transform = state.get("data", {}).get("transform", {})
            verification = verify_transform(
                expected=actor_spec["transform"],
                actual=actual_transform,
            )
        # L3 UI 工具的验证已在交叉比对中完成
        elif exec_method == "ui_tool" and result.get("cross_verification"):
            verification = {
                "status": result["cross_verification"]["final_status"],
                "checks": [],
                "cross_verification": result["cross_verification"],
            }
        else:
            verification = {"status": "failed", "checks": []}

        verification_results.append(verification)

    # Step 7: 副作用检查
    dirty = get_dirty_assets()
    dirty_list = dirty.get("data", {}).get("dirty_assets", [])
    map_result = run_map_check(level_path=target_level)

    # Step 8: 生成报告
    report = generate_report(
        spec_path=spec_path,
        plan=plan,
        execution_results=execution_results,
        verification_results=verification_results,
        dirty_assets=dirty_list,
        map_check=map_result,
    )

    return report
```

---

## 6. 报告输出格式

```yaml
report_version: v0.1
timestamp: "2026-03-19T14:30:00"
spec_path: "Specs/examples/warehouse_test.yaml"
overall_status: success    # 或 mismatch

plan:
  - actor_id: truck_01
    action: CREATE
    reason: "Actor 'truck_01' not found, will create"
  - actor_id: crate_cluster_a
    action: CREATE
    reason: "Actor 'crate_cluster_a' not found, will create"

results:
  - actor_id: truck_01
    execution_status: success
    verification:
      status: success
      checks:
        - field: location
          expected: [0.0, 200.0, 0.0]
          actual: [0.0, 200.0, 0.0]
          tolerance: 0.01
          pass: true
        - field: rotation
          expected: [0.0, 90.0, 0.0]
          actual: [0.0, 90.0, 0.0]
          tolerance: 0.01
          pass: true
        - field: relative_scale3d
          expected: [1.25, 1.25, 1.25]
          actual: [1.25, 1.25, 1.25]
          tolerance: 0.001
          pass: true

  - actor_id: crate_cluster_a
    execution_status: success
    verification:
      status: success
      checks:
        - field: location
          expected: [450.0, -300.0, 0.0]
          actual: [450.0, -300.0, 0.0]
          tolerance: 0.01
          pass: true

side_effects:
  dirty_assets:
    - /Game/Maps/TestMap
  map_errors: []
  map_warnings: []
```

---

## 7. 错误处理

### 7.1 Spec 读取失败

如果 Spec 文件不存在或格式非法，Orchestrator 应直接返回错误，不执行任何操作：

```python
try:
    spec = read_spec(spec_path)
except Exception as e:
    return {
        "overall_status": "failed",
        "error": f"Failed to read spec: {e}",
    }
```

### 7.2 单个 Actor 执行失败

如果某个 Actor 的 spawn/transform 失败，Orchestrator 应：
1. 记录该 Actor 的失败状态
2. **继续执行后续 Actor**（不因一个失败中断全部）
3. 在最终报告中标记 overall_status 为 `mismatch` 或 `failed`

### 7.3 Bridge 函数异常

Bridge 函数已通过 `safe_execute` 包装（见 `bridge_implementation_plan.md`），不会抛出未处理异常。Orchestrator 只需检查返回的 `status` 字段。

---

## 8. 当前限制

### 8.1 当前支持

- 读取 YAML Spec
- CREATE / UPDATE 两种操作
- spawn_actor + set_actor_transform
- 写后读回 + 容差验证
- dirty assets + map check
- 结构化报告

### 8.2 当前不支持（后续扩展）

- 自然语言输入（需要 Spec Translator）
- 自动重试（mismatch 后自动修正）
- 多 Spec 并行执行
- 完整回滚/checkpoint（当前可利用 UE5 Transaction System 的基础 Undo）
- 碰撞/材质操作的编排（Phase 2 接口就绪后扩展）
- 约束求值（must_not_overlap / must_be_inside 的自动判定）
- 进程管理：v0.3 已通过 Gauntlet 实现自动启动/监控/停止 Editor

### 8.3 扩展方向

**第 5 周+**：
- 调用 `validate_actor_inside_bounds` / `validate_actor_non_overlap`
- 将 Spec 中的 `constraints` 映射为验证调用
- 对 mismatch 的 Actor 自动生成修正 Spec
- mismatch 时利用 UE5 Transaction Undo 自动回滚

**v0.3（对接 UE5 官方能力）**：
- ✅ 核心验证逻辑已实装为 UE5 Automation Test / Automation Spec
- ✅ Gauntlet + UAT 实现 CI/CD 无人值守编排
- ✅ L3 Demo 测试已实装为 Functional Test（FTEST_ 地图）
- ✅ 超时机制已通过 safe_execute(timeout) + Gauntlet MaxDuration 实现

**后续扩展**：
- Latent Command 支持（跨帧异步操作的更精细控制）
- 多 Spec 并行执行
- 自动重试（mismatch 后自动修正 + 重跑）

---

## 9. 与路线图的对应

| 周次 | Orchestrator 相关任务 |
|---|---|
| 第 2-3 周 | 手工调用 Bridge 函数跑通闭环（无 Orchestrator） |
| 第 4 周 | 实现 Orchestrator 初版（spec_reader + plan_generator + orchestrator.py） |
| 第 4 周 | 实现 verifier.py + report_generator.py |
| 第 5 周 | 扩展 Orchestrator 接入验证接口（inside_bounds / non_overlap） |
| 第 8 周 | 用 Orchestrator 跑完整 Demo 场景 |

