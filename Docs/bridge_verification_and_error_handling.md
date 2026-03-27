# Bridge 接口验证与错误处理手册

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 文档目的

本文档回答两个问题：

1. **每个 Bridge 函数实现后，怎么算"做完了"？**（接口验证）
2. **Bridge 在异常场景下应该怎么表现？**（错误处理）

本文档与冒烟测试方案（`mvp_smoke_test_plan.md`）的区别：冒烟测试关注"端到端链路是否通"，本文档关注"每个函数的实现质量是否达标"和"异常路径是否被正确处理"。

**三通道注意**：Bridge 封装层支持三通道（通道 A: Python Editor Scripting / 通道 B: Remote Control API / 通道 C: C++ Plugin）。v0.3 中 C++ Plugin（通道 C）是"权威实现"——参数校验、错误处理、Transaction 管理全部在 C++ 端完成。通道 A/B 作为客户端，验证时以通道 C 的行为为基准。L1/L2/L3 测试在 AgentBridgeTests Plugin 中通过通道 C 直接调用 Subsystem 进行验证。

---

# 第一部分：接口验证

---

## 2. 验证总则

### 2.1 每个接口的验证三步

| 步骤 | 说明 |
|---|---|
| **结构验证** | 返回值 JSON 结构是否符合对应 Schema |
| **语义验证** | 返回的数据是否与 UE5.5.4 Editor 中的真实状态一致 |
| **边界验证** | 异常输入（空路径、不存在的对象、Editor 未就绪）是否返回正确的错误码 |

### 2.2 结构验证方法

对每个接口返回值执行 Schema 校验：

```python
import json
from jsonschema import Draft202012Validator, RefResolver
from pathlib import Path

def validate_response(response: dict, schema_path: str) -> bool:
    """将 Bridge 返回值与对应 Schema 校验。"""
    schema_file = Path(schema_path)
    schema = json.loads(schema_file.read_text(encoding="utf-8"))
    resolver = RefResolver(base_uri=schema_file.resolve().as_uri(), referrer=schema)
    validator = Draft202012Validator(schema, resolver=resolver)
    errors = list(validator.iter_errors(response))
    if errors:
        for e in errors:
            print(f"  Schema error: {e.message} at {'.'.join(str(p) for p in e.absolute_path)}")
        return False
    return True
```

开发者在实现每个 Bridge 函数后，应立即调用此方法验证返回值。

### 2.3 "做完"的定义

一个 Bridge 函数被认为"做完"，当且仅当：

1. Happy path 返回值通过对应 Schema 校验
2. `status` 字段使用正确的枚举值（success / warning / failed）
3. 至少覆盖一个错误路径（如 ACTOR_NOT_FOUND），返回 `status: "failed"` + 结构化 `errors`
4. 返回的数据与 UE5.5.4 Editor 中的真实状态一致（人工在 Editor 中核对）

---

## 3. 逐接口验证清单

### 3.1 反馈接口

| 接口 | Schema 路径 | 关键验证点 | 错误路径验证 |
|---|---|---|---|
| `get_current_project_state` | `feedback/project/get_current_project_state.response.schema.json` | engine_version 含 "5.5"；editor_mode 在枚举内；current_level 与 Editor 中一致 | Editor 未启动时返回 `EDITOR_NOT_READY` |
| `list_level_actors` | `feedback/level/list_level_actors.response.schema.json` | actors 数量与 Editor 中 World Outliner 一致；每个元素有 actor_name / actor_path / class | 空关卡返回空数组（非 error） |
| `get_actor_state` | `feedback/actor/get_actor_state.response.schema.json` | transform 与 Editor Details Panel 中一致；collision 字段完整（如有） | 不存在的 actor_path → `ACTOR_NOT_FOUND` |
| `get_actor_bounds` | `feedback/actor/get_actor_bounds.response.schema.json` | bounds 与 Editor 中 Show → Bounds 显示一致 | 不存在的 actor_path → `ACTOR_NOT_FOUND` |
| `get_asset_metadata` | `feedback/asset/get_asset_metadata.response.schema.json` | exists 值与 Content Browser 一致；class 正确 | 不存在的资产 → exists=false（非 error） |
| `get_dirty_assets` | `feedback/asset/get_dirty_assets.response.schema.json` | 列表与 Editor 中 File → Save All 弹出的脏资产一致 | 无脏资产 → 空数组（非 error） |
| `run_map_check` | `feedback/validation/run_map_check.response.schema.json` | map_errors / map_warnings 与 Editor Message Log 中 MapCheck 输出一致 | 空关卡 → 空数组 |

### 3.2 写接口

| 接口 | Schema 路径 | 关键验证点 | 错误路径验证 |
|---|---|---|---|
| `spawn_actor` | `write_feedback/write_operation_feedback.response.schema.json` | Actor 确实出现在关卡中；actual_transform 与 Editor Details Panel 一致；dirty_assets 含关卡路径 | class 不存在 → `CLASS_NOT_FOUND`；级别未加载 → `LEVEL_NOT_FOUND` |
| `set_actor_transform` | 同上 | old_transform 与修改前一致；actual_transform 与修改后 Editor 中一致 | actor 不存在 → `ACTOR_NOT_FOUND` |
| `import_assets` | 同上 | 资产出现在 Content Browser；created_objects 路径正确 | 源文件不存在 → `INVALID_ARGS`；目标路径无效 → `INVALID_ARGS` |
| `create_blueprint_child` | 同上 | Blueprint 出现在 Content Browser；父类正确 | 父类不存在 → `CLASS_NOT_FOUND` |

### 3.3 验证执行记录模板

每个接口验证完成后，记录以下信息：

```yaml
interface_verification:
  function: get_actor_state
  date: "2026-03-19"
  schema_validation: PASS      # 返回值通过 Schema 校验
  semantic_validation: PASS     # 数据与 Editor 真实状态一致
  error_path_tested:
    - scenario: actor_not_found
      input: { actor_path: "/Game/Maps/TestMap.TestMap:PersistentLevel.NonExistent" }
      expected_status: failed
      expected_error_code: ACTOR_NOT_FOUND
      actual_status: failed
      actual_error_code: ACTOR_NOT_FOUND
      result: PASS
  overall: PASS
  notes: ""
```

---

## 4. Mock 模式

### 4.1 为什么需要 Mock

开发初期（尤其是 Bridge 代码尚未接入真实 UE5.5.4 Editor 时），可以使用 Mock 模式：

- Bridge 函数返回预定义的 mock 数据（即 `PythonPackage/agent_ue5/schemas/examples/` 中的 example JSON）
- Orchestrator / 验证层可以先用 mock 数据跑通流程
- 后续再将 mock 替换为真实 UE API 调用

### 4.2 Mock 实现方式

```python
# bridge_core.py 中增加 mock 支持

import json
from pathlib import Path

MOCK_MODE = False  # 全局开关，开发时设为 True
PACKAGE_ROOT = Path(__file__).resolve().parents[1]
EXAMPLES_DIR = PACKAGE_ROOT / "schemas" / "examples"

MOCK_MAP = {
    "get_current_project_state": "get_current_project_state.example.json",
    "list_level_actors": "list_level_actors.example.json",
    "get_actor_state": "get_actor_state.example.json",
    "get_actor_bounds": "get_actor_bounds.example.json",
    "get_asset_metadata": "get_asset_metadata.example.json",
    "get_dirty_assets": "get_dirty_assets.example.json",
    "run_map_check": "run_map_check.example.json",
    "write_operation_feedback": "write_operation_feedback.example.json",
}

def get_mock_response(tool_name: str) -> dict:
    """返回对应 example JSON 作为 mock 响应。"""
    filename = MOCK_MAP.get(tool_name)
    if not filename:
        return make_response(
            status="failed",
            summary=f"No mock available for {tool_name}",
            data={},
            errors=[make_error("TOOL_EXECUTION_FAILED", f"Mock not found: {tool_name}")],
        )
    example_path = EXAMPLES_DIR / filename
    return json.loads(example_path.read_text(encoding="utf-8"))
```

### 4.3 Mock 使用规则

- Mock 模式**仅用于开发和调试**，不用于验收
- Mock 返回的数据必须通过 Schema 校验（example 本身已通过校验）
- 切换到真实模式时，将 `MOCK_MODE` 设为 `False`
- 冒烟测试（L1/L2/L3）必须在真实模式下执行

---

# 第二部分：错误处理

---

## 5. 错误处理总则

### 5.1 统一错误响应结构

所有 Bridge 函数在发生错误时，必须返回统一结构（不能抛异常、不能返回 None、不能返回非标准格式）：

```json
{
  "status": "failed",
  "summary": "人可读的简短错误描述",
  "data": {},
  "warnings": [],
  "errors": [
    {
      "code": "ACTOR_NOT_FOUND",
      "message": "No actor found at the specified path",
      "details": {
        "actor_path": "/Game/Maps/TestMap.TestMap:PersistentLevel.NonExistent"
      }
    }
  ]
}
```

### 5.2 绝不抛出未处理异常

Bridge 函数的顶层必须使用 `safe_execute` 包装（见 `bridge_implementation_plan.md` 第 4.1 节），确保 UE Python API 的异常被捕获并转换为结构化 `failed` 响应。

### 5.3 错误 status 选择

| 场景 | status |
|---|---|
| 输入参数格式错误（缺字段、类型不对） | `validation_error` |
| 目标对象不存在 | `failed`（配 ACTOR_NOT_FOUND / ASSET_NOT_FOUND） |
| Editor 环境问题 | `failed`（配 EDITOR_NOT_READY） |
| 工具执行异常（UE API 抛错） | `failed`（配 TOOL_EXECUTION_FAILED） |
| 执行成功但读回值与预期不符 | `mismatch` |
| 执行成功但有非阻塞警告 | `warning` |
| 执行成功 | `success` |

---

## 6. 错误码映射表

### 6.1 输入类错误

| 错误码 | 触发条件 | 示例 |
|---|---|---|
| `INVALID_ARGS` | 必填参数缺失、类型不对、格式非法 | actor_path 为空字符串；transform 缺少 location |
| `DRY_RUN_REQUIRED` | 高风险操作未设置 dry_run=true | 批量删除未 dry-run（后续扩展） |

### 6.2 对象类错误

| 错误码 | 触发条件 | 示例 |
|---|---|---|
| `ACTOR_NOT_FOUND` | 指定 actor_path 在关卡中不存在 | get_actor_state 传入无效路径 |
| `ASSET_NOT_FOUND` | 指定 asset_path 不存在 | import 后查询不存在的路径 |
| `LEVEL_NOT_FOUND` | 指定关卡未加载或不存在 | spawn_actor 指定未加载的关卡 |
| `CLASS_NOT_FOUND` | 指定类路径无法加载 | spawn_actor 传入无效 class |
| `PROPERTY_NOT_ALLOWED` | 尝试修改非白名单属性 | set_asset_properties 修改受限属性（后续扩展） |

### 6.3 环境类错误

| 错误码 | 触发条件 | 示例 |
|---|---|---|
| `EDITOR_NOT_READY` | Editor 未启动、正在加载、或处于不兼容模式 | PIE 模式下调用写接口 |
| `BUILD_FAILED` | 构建失败 | build_project 返回编译错误（后续扩展） |
| `SAVE_FAILED` | 保存操作失败 | save_named_assets 遇到只读文件 |

### 6.4 执行类错误

| 错误码 | 触发条件 | 示例 |
|---|---|---|
| `TOOL_EXECUTION_FAILED` | UE Python API 抛出未预期异常 | unreal.EditorLevelLibrary 方法内部错误 |
| `WRITE_SCOPE_EXCEEDED` | 工具修改了声明范围外的对象 | spawn_actor 意外修改了其他 Actor（理论上不应发生） |
| `READBACK_FAILED` | 写后读回失败（能写入但无法读回） | spawn 成功但 get_actor_location 返回异常 |
| `VALIDATION_FAILED` | 写后验证未通过 | spawn_actor 后 bounds 检查不通过 |

### 6.5 L3 UI 工具类错误

| 错误码 | 触发条件 | 示例 |
|---|---|---|
| `DRIVER_NOT_AVAILABLE` | Automation Driver 模块未加载或未启用 | L3 接口调用时 AutomationDriver Plugin 未启用 |
| `WIDGET_NOT_FOUND` | 目标 UI Widget 未找到（按钮/输入框/面板不存在或不可见） | ClickDetailPanelButton 传入不存在的按钮文本 |
| `UI_OPERATION_TIMEOUT` | Automation Driver 操作未在超时时间内完成 | DragAssetToViewport 后 UI 未回到 Idle 状态 |

L3 错误码在 `BridgeTypes.h` 的 `EBridgeErrorCode` 枚举中定义，对应的字符串值为 `DRIVER_NOT_AVAILABLE` / `WIDGET_NOT_FOUND` / `UI_OPERATION_TIMEOUT`。L3 接口在 Automation Driver 不可用时返回 `DRIVER_NOT_AVAILABLE`（graceful degradation），不会崩溃或阻塞。

---

## 7. 边界条件清单

### 7.1 环境边界条件

| 条件 | 预期行为 | 错误码 |
|---|---|---|
| Editor 未启动 | 返回 failed | `EDITOR_NOT_READY` |
| Editor 处于 PIE 模式 | 写接口返回 failed；查询接口返回 warning（数据可能不准确） | `EDITOR_NOT_READY`（写）/ warning（读） |
| Editor 正在编译 Shader | 查询接口正常返回；写接口可能阻塞 | 加超时处理 |
| 无关卡加载 | list_level_actors 返回空数组；spawn_actor 返回 failed | `LEVEL_NOT_FOUND` |

### 7.2 输入边界条件

| 条件 | 预期行为 | 错误码 |
|---|---|---|
| actor_path 为空字符串 | 返回 failed | `INVALID_ARGS` |
| actor_path 格式正确但 Actor 不存在 | 返回 failed | `ACTOR_NOT_FOUND` |
| asset_path 为空字符串 | 返回 failed | `INVALID_ARGS` |
| asset_path 指向不存在的资产 | get_asset_metadata 返回 exists=false；其他接口返回 failed | — / `ASSET_NOT_FOUND` |
| transform 缺少 location 字段 | 返回 failed | `INVALID_ARGS` |
| transform.location 不是 3 元素数组 | 返回 failed | `INVALID_ARGS` |
| transform.location 包含非数字 | 返回 failed | `INVALID_ARGS` |
| class 路径无效 | 返回 failed | `CLASS_NOT_FOUND` |
| actor_name 包含 UE5 不允许的字符 | 返回 failed | `INVALID_ARGS` |
| 同名 Actor 已存在 | spawn_actor 返回 warning（UE5 会自动重命名） | `warning` status + 说明 |

### 7.3 数据边界条件

| 条件 | 预期行为 | 说明 |
|---|---|---|
| 关卡中有 0 个 Actor | list_level_actors 返回空数组 | 正常行为，不是错误 |
| 关卡中有 10000+ 个 Actor | list_level_actors 正常返回 | 可能较慢，考虑后续加分页 |
| Actor transform 含极大值 | 正常返回实际值 | UE5 允许大坐标 |
| Actor bounds extent 为 0 | 正常返回 | 某些 Actor（如空 Actor）bounds 可能为零 |
| 无 dirty assets | get_dirty_assets 返回空数组 | 正常行为 |
| collision 为 None（Actor 无碰撞组件） | get_actor_state 的 collision 字段为空对象 `{}` | 不是错误 |

---

## 8. 输入验证实现

建议在每个 Bridge 函数入口处做输入验证，统一使用 `validation_error` status：

```python
# bridge_core.py 中增加

def validate_required_string(value, field_name: str) -> Optional[dict]:
    """验证必填字符串参数。返回 None 表示通过，返回 dict 表示错误响应。"""
    if not value or not isinstance(value, str) or len(value.strip()) == 0:
        return make_response(
            status="validation_error",
            summary=f"Missing or invalid required field: {field_name}",
            data={},
            errors=[make_error("INVALID_ARGS", f"{field_name} must be a non-empty string")],
        )
    return None


def validate_transform(transform: dict) -> Optional[dict]:
    """验证 transform 参数格式。"""
    required_keys = ["location", "rotation", "relative_scale3d"]
    for key in required_keys:
        if key not in transform:
            return make_response(
                status="validation_error",
                summary=f"Transform missing required field: {key}",
                data={},
                errors=[make_error("INVALID_ARGS", f"transform.{key} is required")],
            )
        val = transform[key]
        if not isinstance(val, list) or len(val) != 3:
            return make_response(
                status="validation_error",
                summary=f"transform.{key} must be a 3-element number array",
                data={},
                errors=[make_error("INVALID_ARGS", f"transform.{key} must be [x, y, z]")],
            )
        if not all(isinstance(v, (int, float)) for v in val):
            return make_response(
                status="validation_error",
                summary=f"transform.{key} contains non-numeric values",
                data={},
                errors=[make_error("INVALID_ARGS", f"transform.{key} values must be numbers")],
            )
    return None
```

使用方式：

```python
def spawn_actor(level_path, actor_class, actor_name, transform, dry_run=False):
    # 输入验证
    err = validate_required_string(level_path, "level_path")
    if err: return err

    err = validate_required_string(actor_class, "actor_class")
    if err: return err

    err = validate_required_string(actor_name, "actor_name")
    if err: return err

    err = validate_transform(transform)
    if err: return err

    # ... 正常逻辑
```

---

## 9. Editor 状态检查

建议在每个写接口的入口处检查 Editor 状态：

```python
def check_editor_ready() -> Optional[dict]:
    """检查 Editor 是否处于可写状态。"""
    try:
        world = unreal.EditorLevelLibrary.get_editor_world()
        if world is None:
            return make_response(
                status="failed",
                summary="No editor world available",
                data={},
                errors=[make_error("EDITOR_NOT_READY", "Editor world is None, no level loaded")],
            )

        # 检查是否在 PIE 模式
        game_world = unreal.EditorLevelLibrary.get_game_world()
        if game_world is not None:
            return make_response(
                status="failed",
                summary="Editor is in PIE mode, write operations not allowed",
                data={},
                errors=[make_error("EDITOR_NOT_READY",
                                   "Cannot perform write operations during Play In Editor")],
            )

    except Exception as e:
        return make_response(
            status="failed",
            summary=f"Editor state check failed: {e}",
            data={},
            errors=[make_error("EDITOR_NOT_READY", str(e))],
        )

    return None  # Editor 就绪
```

写接口使用方式：

```python
def spawn_actor(...):
    err = check_editor_ready()
    if err: return err
    # ... 正常逻辑
```

查询接口在 PIE 模式下可继续执行，但应在 warnings 中提示数据可能不准确。

---

## 9.5 Remote Control API（通道 B）错误处理

通过 Remote Control API 执行操作时，错误来源与通道 A 不同：

### HTTP 错误码映射

| HTTP 状态码 | 含义 | 映射到 Bridge 错误码 |
|---|---|---|
| 200 | 成功 | `status: "success"` |
| 400 | 请求格式错误 | `INVALID_ARGS` |
| 404 | objectPath 不存在 | `ACTOR_NOT_FOUND` 或 `ASSET_NOT_FOUND` |
| 500 | 引擎内部错误 | `TOOL_EXECUTION_FAILED` |
| 连接被拒绝 | Editor 未启动或 RC Plugin 未启用 | `EDITOR_NOT_READY` |
| 超时 | 请求超时 | `TOOL_EXECUTION_FAILED` + timeout 说明 |

### 属性名转换

Remote Control API 使用 **PascalCase**（UE5 C++ 风格），Bridge 封装层使用 **snake_case**（Python 风格）：

| Remote Control 属性名 | Bridge 字段名 |
|---|---|
| `RelativeLocation` | `location` |
| `RelativeRotation` | `rotation` |
| `RelativeScale3D` | `relative_scale3d` |
| `CollisionProfileName` | `collision_profile_name` |

Bridge 封装层负责此转换。如果转换错误，通道 B 的返回值将无法通过 Schema 校验。

### Transaction / Undo 验证

通过通道 B 执行写操作时，如果设置了 `generateTransaction: true`：

1. 操作完成后，在 Editor 中执行 Ctrl+Z 应能撤销该操作
2. Bridge 返回值中应包含 `"transaction": true` 标记
3. 如果 Undo 后读回的状态与 Undo 前不同，说明 Transaction 生效

**验证步骤**：
```
1. spawn_actor via 通道 B (generateTransaction=true)
2. 记录 actor_path
3. get_actor_state 确认 Actor 存在
4. Editor 中 Ctrl+Z（或程序化调用 Undo）
5. get_actor_state 确认 Actor 不存在 → Transaction 生效
```

---

## 10. 与其他文档的关系

| 文档 | 关系 |
|---|---|
| `AGENTS.md` 2.7 节 | status 枚举的定义来源 |
| `Tool Contract` 2.3-2.4 节 | 错误码的定义来源 |
| `agent_ue5/schemas/common/error.schema.json` | 错误对象的 Schema 定义 |
| `agent_ue5/schemas/common/primitives.schema.json` | status 枚举的 Schema 定义（唯一真理来源） |
| `bridge_implementation_plan.md` | Bridge 函数的三通道实现方案（C++ Plugin 核心） |
| `mvp_smoke_test_plan.md` | 测试方案（L1/L2/L3 Automation Test 驱动） |
| `ue5_capability_map.md` | UE5 官方能力总览（每个工具的 UE5 依赖来源） |

---

## 11. 检查清单总表

开发者在实现每个 Bridge 函数后，应逐项确认：

| # | 检查项 | 通过标准 |
|---|---|---|
| 1 | Happy path 返回值通过 Schema 校验 | `validate_response()` 返回 True |
| 2 | status 使用正确枚举值 | 在 {success, warning, failed, mismatch, validation_error} 中 |
| 3 | 数据与 Editor 真实状态一致 | 人工在 Editor 中核对 |
| 4 | 输入验证覆盖（空值 / 类型错误 / 缺字段） | 返回 `validation_error` + `INVALID_ARGS` |
| 5 | 对象不存在场景覆盖 | 返回 `failed` + 正确错误码 |
| 6 | Editor 未就绪场景覆盖 | 写接口返回 `failed` + `EDITOR_NOT_READY` |
| 7 | 无未处理异常 | 使用 `safe_execute` 包装或手动 try-except |
| 8 | warnings 正确使用 | 非阻塞问题放 warnings，不放 errors |
| 9 | dirty_assets 正确返回 | 写操作后 dirty_assets 包含受影响的资产/关卡路径 |
| 10 | actual_* 为真实读回值 | 不是简单复制输入参数，而是从 UE5 API 重新读取 |
| 11 | **C++ Plugin 实现通过 L1 测试** | Session Frontend 中对应的 L1 测试为绿灯 |
| 12 | **三通道一致性** | 通道 A / B / C 的返回值结构一致，均通过同一 Schema 校验 |
| 13 | **通道 B/C 属性名转换** | PascalCase（UE5 C++）→ snake_case（Python 端）正确 |
| 14 | **写操作 Transaction 生效** | C++ FScopedTransaction（通道 C）/ generateTransaction（通道 B）→ Ctrl+Z 可撤销 |
| 15 | **L2 闭环通过** | 对应的 L2 Automation Spec 全部 It 为绿灯 |
