# Schemas

> 目标引擎版本：UE5.5.4 | Schema 版本：v0.1

## 1. 目录用途

`Schemas/` 是 AGENT + UE5 可操作层的数据协议层（Schema Repository）。

它不直接执行 UE5 操作，但约束所有通信层的数据格式：工具输入参数、反馈接口输出、写后反馈结构、example JSON。

**Schema 是编排层的自有契约——UE5 官方没有 JSON Schema 体系。** 这是本方案在 UE5 原生 API 之上增加的核心价值之一。Bridge 封装层的每个函数，无论通过通道 A（Python）、通道 B（Remote Control API）还是通道 C（C++ Plugin），其返回值都必须符合对应 Schema 的定义。

**v0.3 中 Schema 与 C++ Plugin 的关系**：C++ Plugin 中的 `FBridgeResponse::ToJsonString()` 输出的 JSON 必须通过 Schema 校验。Schema 是"契约"，C++ Plugin 是"实现"——两者通过 JSON 格式对齐。Schema 校验（`validate_examples.py`）保留在 Python 端，因为 UE5 C++ 没有 JSON Schema 验证库。

与其他核心文档的关系：

| 文档 | 职责 |
|---|---|
| `AGENTS.md` | Agent 行为规则 |
| `Tool Contract` | 工具职责与参数契约（标注 UE5 依赖） |
| `Schemas/` | 工具输入输出的数据结构契约（编排层自有，UE5 无对应物） |
| `ue5_capability_map.md` | UE5 官方能力总览及与本方案的映射关系 |

---

## 2. 目录结构

```
Schemas/
├── README.md              # 本文件
├── common/                # 通用基础 Schema
│   ├── primitives.schema.json    # status / vector3 / rotator / string_array
│   ├── transform.schema.json     # transform 结构
│   ├── bounds.schema.json        # bounds / mesh_asset_bounds
│   ├── collision.schema.json     # collision_state
│   ├── error.schema.json         # warnings_array / errors_array
│   └── material.schema.json      # material_slot_assignment
├── feedback/              # 只读反馈接口 Schema
│   ├── project/
│   ├── level/
│   ├── actor/
│   ├── asset/
│   └── validation/
├── write_feedback/        # 写操作后的反馈 Schema
├── examples/              # 与 Schema 对应的 example JSON（平铺）
└── versions/              # Schema 版本清单
    └── v0.1_manifest.json
```

---

## 3. 核心规则

### 3.1 不使用 `$id`

**首批 Schema 全部不使用 `$id`。** 这是硬规则。

原因：`$id` 会覆盖 JSON Schema 的 base URI，导致 `$ref` 中的相对路径被解析为远程 URL（如 `https://example.com/...`），在离线环境、CI 环境中触发联网/证书错误，阻塞本地校验链。

所有 `$ref` 使用纯相对路径（相对于当前 Schema 文件位置），由 validate_examples.py 的 RefResolver 基于文件系统路径解析。

如未来需要 `$id`，仅在独立 Schema registry 场景下引入，并需同步更新校验脚本。

### 3.2 统一响应外壳

所有反馈接口和写后反馈必须返回：

```json
{
  "status": "...",
  "summary": "...",
  "data": { ... },
  "warnings": [],
  "errors": []
}
```

### 3.3 status 枚举（5 值）

| 值 | 含义 |
|---|---|
| `success` | 执行成功，验证通过 |
| `warning` | 执行成功，有非阻塞警告 |
| `failed` | 执行失败 |
| `mismatch` | 执行成功但读回值与预期不符 |
| `validation_error` | 参数/Spec 校验未通过，未执行 |

唯一真理来源：`common/primitives.schema.json#/$defs/status`

### 3.4 bounds 结构规则

- **独立 bounds 接口**（如 get_actor_bounds）：data 下直接平铺 `world_bounds_origin` / `world_bounds_extent`（因为 data 本身就是 bounds）
- **写后反馈**（如 write_operation_feedback）：嵌套在 `data.actual_bounds` 对象下

### 3.5 additionalProperties

所有 Schema 默认使用 `"additionalProperties": false`，禁止未声明的字段通过校验。唯一例外是 `validation` 对象（当前为 `additionalProperties: true`，因验证结构尚未完全固定）。

---

## 4. 统一字段命名约定

### Transform

`location`（cm, `[x,y,z]`）/ `rotation`（degrees, `[pitch,yaw,roll]`）/ `relative_scale3d`（倍率, `[x,y,z]`）

### Bounds

`world_bounds_origin`（cm）/ `world_bounds_extent`（cm）/ `mesh_asset_bounds`

### Collision

`collision_profile_name` / `collision_box_extent`（cm, 仅 BoxComponent）/ `collision_enabled` / `generate_overlap_events` / `can_affect_navigation` / `collision_capsule_radius` / `collision_capsule_half_height`

### Result

`actual_transform` / `actual_bounds` / `actual_collision` / `dirty_assets` / `warnings` / `errors`

### 禁止字段

以下字段不应出现在 Schema 或 example 中：`size` / `position` / `center` / `near` / `looks_good` / `proper_scale`

---

## 5. 命名规则

| 类型 | 命名模式 | 示例 |
|---|---|---|
| 查询反馈 Schema | `<tool_name>.response.schema.json` | `get_actor_state.response.schema.json` |
| 写后反馈 Schema | `<tool_name>_feedback.response.schema.json` 或通用 `write_operation_feedback.response.schema.json` | — |
| Example | `<tool_name>.example.json` | `get_actor_state.example.json` |
| Common | `<concept>.schema.json` | `primitives.schema.json` |

---

## 6. 当前 v0.1 文件清单

### Common（6 个）

`primitives` / `transform` / `bounds` / `collision` / `error` / `material`

### Feedback（9 个）

`get_current_project_state` / `list_level_actors` / `get_actor_state` / `get_actor_bounds` / `get_component_state` / `get_material_assignment` / `get_asset_metadata` / `get_dirty_assets` / `run_map_check`

### Write Feedback（1 个）

`write_operation_feedback`

### Examples（10 个）

与上述 9 个 feedback + 1 个 write_feedback 一一对应。

### 校验状态

`validate_examples.py --strict` → 10 checked / 10 passed / 0 failed ✅

---

## 7. 校验链使用说明

### 运行全量校验

```bash
cd ProjectRoot
python Scripts/validation/validate_examples.py --strict
```

预期输出：8 passed / 0 failed / 0 unmapped / 0 missing

### 校验单个 example

```bash
python Scripts/validation/validate_examples.py --example get_actor_state.example.json
```

### 查看映射关系

```bash
python Scripts/validation/validate_examples.py --list
```

### 新增接口的校验流程

1. 在对应目录创建 `<tool_name>.response.schema.json`（不使用 `$id`）
2. 在 `examples/` 创建 `<tool_name>.example.json`（status 使用 `"success"`）
3. 在 `validate_examples.py` 的 `EXAMPLE_TO_SCHEMA` 中增加映射
4. 运行 `validate_examples.py --strict` 确认零错误
5. 更新 `versions/v0.1_manifest.json`
