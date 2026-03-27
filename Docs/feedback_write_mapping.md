# 反馈接口 + 写接口一一对应关系表

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AGENT + UE5 可操作层

## 1. 文档目的

本文档定义：**每个写操作做完后，至少要靠哪些反馈接口，才能证明它真的做对了。**

写接口负责"动手"，反馈接口负责"验收"。

---

## 2. 总体规则

### 2.1 每个写接口都必须有最小验收闭环

至少回答四个问题：

| 问题 | 对应字段 / 接口 | 判定方式 |
|---|---|---|
| 有没有执行成功 | `status` | success / failed |
| 作用到谁了 | `created_objects` / `modified_objects` + `actor_path` | 非空 |
| 最终状态是不是目标状态 | `actual_*` vs Spec 中的 expected | success / mismatch（含容差比对） |
| 有没有产生意外副作用 | `dirty_assets` + `warnings` + `run_map_check` | 无意外条目 |

### 2.2 优先"写后直接读回"，再补独立验证

1. 写接口自身返回 `actual_*`（第一次读回）
2. 调用独立反馈接口做二次确认（第二次读回）

### 2.3 反馈分三层

| 层 | 职责 | 典型接口 |
|---|---|---|
| **存在性确认** | 对象是否真的存在 / 被改了 | `list_level_actors` / `get_asset_metadata`（exists） |
| **状态确认** | 当前值是否等于目标值 | `get_actor_state` / `get_actor_bounds` |
| **副作用确认** | 是否引发脏资源、越界、重叠、日志错误 | `get_dirty_assets` / `run_map_check` / `get_editor_log_tail` |

### 2.4 强制规则

1. **任何写接口都必须声明自己的最小反馈集合。**
2. **没有对应反馈闭环的写接口，不允许进入自动执行。**
3. **高风险写接口至少需要存在性确认 + 状态确认 + 副作用确认三层反馈全部具备。**
4. **即使写接口已返回 `actual_*`，仍须至少调用一个独立反馈接口做二次确认。**

---

## 3. 验证判定标准

### 3.1 精确匹配

适用于字符串和 boolean 字段：`actor_path` / `asset_path` / `exists` / `collision_profile_name`

判定：actual == expected → success；否则 → mismatch

### 3.2 容差匹配

适用于浮点数字段：`location` / `rotation` / `relative_scale3d` / `world_bounds_extent`

| 字段 | 容差 |
|---|---|
| location | ≤ 0.01 cm |
| rotation | ≤ 0.01 degrees |
| relative_scale3d | ≤ 0.001 |
| world_bounds_extent | ≤ 1.0 cm |

判定：|actual - expected| ≤ tolerance → success；否则 → mismatch

### 3.3 非空判定

适用于列表字段：`dirty_assets` / `created_objects`

判定：写操作后 dirty_assets 应包含目标资产路径；created_objects 应非空。

---

## 4. 核心闭环（4 组）

这 4 组闭环中所有反馈接口均已有 Tool Contract 定义和 Schema。

### 4.1 import_assets

| 层 | 反馈接口 | 验证目标 |
|---|---|---|
| 存在性 | `get_asset_metadata`（exists=true） | 资产是否真的存在 |
| 状态 | `get_asset_metadata`（class / asset_path） | 类型和路径是否正确 |
| 副作用 | `get_dirty_assets` + `get_editor_log_tail` | 是否产生脏资产、是否有导入错误 |

**最低闭环步骤**：
1. 写接口返回 created_objects（asset_path）
2. `get_asset_metadata` 确认 exists=true
3. `get_dirty_assets` 确认有修改
4. `get_editor_log_tail` 检查导入日志

---

### 4.2 create_blueprint_child

| 层 | 反馈接口 | 验证目标 |
|---|---|---|
| 存在性 | `get_asset_metadata`（exists=true） | 蓝图是否创建成功 |
| 状态 | `get_asset_metadata`（class） | 类型是否正确 |
| 副作用 | `get_dirty_assets` | 是否留下未保存修改 |

**最低闭环步骤**：
1. 写接口返回 created_objects（asset_path）
2. `get_asset_metadata` 确认 exists=true、class 正确
3. `get_dirty_assets` 确认有修改

---

### 4.3 spawn_actor

这是最关键的一组闭环。

| 层 | 反馈接口 | 验证目标 |
|---|---|---|
| 存在性 | `list_level_actors` | Actor 是否出现在关卡中 |
| 状态 | `get_actor_state`（transform）+ `get_actor_bounds` | 位置/缩放是否正确、占地是否合理 |
| 副作用 | `get_dirty_assets` | 地图是否被修改 |

**最低闭环步骤**：
1. 写接口返回 actor_path + actual_transform
2. `list_level_actors` 确认对象存在
3. `get_actor_state` 确认 transform（容差匹配）
4. `get_actor_bounds` 确认占地
5. `get_dirty_assets` 确认地图被修改

---

### 4.4 set_actor_transform

| 层 | 反馈接口 | 验证目标 |
|---|---|---|
| 存在性 | （Actor 已存在，无需确认） | — |
| 状态 | `get_actor_state`（transform）+ `get_actor_bounds` | 新 transform 是否生效、占地是否变化 |
| 副作用 | `get_dirty_assets` | 是否产生修改 |

**最低闭环步骤**：
1. 写接口返回 old_transform + actual_transform
2. `get_actor_state` 二次确认 transform（容差匹配）
3. `get_actor_bounds` 确认占地变化
4. `get_dirty_assets` 确认修改

---

## 5. Phase 2 闭环（2 组）

以下闭环的**写接口**已有 Tool Contract 定义，但部分**反馈接口**尚无 Tool Contract。

### 5.1 set_actor_collision

| 层 | 反馈接口 | 验证目标 | Tool Contract |
|---|---|---|---|
| 存在性 | （Actor 已存在） | — | — |
| 状态 | `get_actor_state`（collision 字段已扩展为完整返回） | 碰撞配置是否生效 | ✅ 已定义 |
| 副作用 | `get_dirty_assets` + `run_map_check` | 是否影响地图健康状态 | ✅ 已定义 |

**说明**：由于 get_actor_state 已扩展为返回完整 collision 信息（含 collision_box_extent / can_affect_navigation），此闭环可通过 get_actor_state 读回验证，不强依赖 get_component_state。

---

### 5.2 assign_material

| 层 | 反馈接口 | 验证目标 | Tool Contract |
|---|---|---|---|
| 存在性 | （Actor 已存在） | — | — |
| 状态 | `get_material_assignment` | 材质是否挂在正确组件和槽位 | ❌ **待补充** |
| 副作用 | `get_dirty_assets` | 是否产生修改 | ✅ 已定义 |

**说明**：此闭环依赖 get_material_assignment 接口，该接口的 Tool Contract 和 Schema 需在 Phase 2 开始前补充。截图（capture_viewport_screenshot）可辅助人工确认但不作为自动化验证手段。

---

## 6. 后续扩展参考映射

以下写接口在当前版本 **Tool Contract 未定义**，Agent **不可调用**。仅为后续版本预留参考。

| 写接口 | 最低必需反馈 | 状态 |
|---|---|---|
| `save_named_assets` | `get_dirty_assets` | Tool Contract 已定义 |
| `replace_selected_actors` | `list_level_actors` + `get_actor_state` + `get_dirty_assets` | Tool Contract 未定义 |
| `batch_rename_assets` | `get_asset_metadata` + `get_dirty_assets` | Tool Contract 未定义 |
| `delete_assets` | `get_asset_metadata` + `get_dirty_assets` | Tool Contract 未定义 |
| `bulk_replace_references` | `get_dirty_assets` + `get_editor_log_tail` | Tool Contract 未定义 |
| `modify_many_level_actors` | `list_level_actors` + `get_actor_state` + `get_dirty_assets` | Tool Contract 未定义 |
| `rewrite_blueprint_graph` | `get_asset_metadata` + `get_editor_log_tail` + `get_dirty_assets` | Tool Contract 未定义 |

---

## 7. 最低闭环模板

所有写接口设计时可套用以下模板：

### 写接口返回

- `status`
- `created_objects` / `modified_objects` / `deleted_objects`
- `actual_*`（transform / bounds / collision / assigned）
- `dirty_assets`

### 三层反馈

1. **存在性确认**：对象是否真的存在 / 被创建 / 被修改
2. **状态确认**：当前值是否等于目标值（含容差）
3. **副作用确认**：是否引发脏资源、越界、重叠、日志错误

---

## 8. 回滚能力

### 8.1 UE5 Transaction System（原生 Undo）

UE5 提供了原生的 Transaction/Undo 系统。v0.3 中通过 C++ Plugin（通道 C）执行的写操作，由 `FScopedTransaction` 自动纳入 UE5 的 Undo/Redo 栈——作用域结束时自动提交，异常时自动回滚。通过 Remote Control API（通道 B）可设置 `generateTransaction: true` 达到相同效果。

这意味着：
- `spawn_actor`（通道 C / 通道 B）后可通过 Undo 撤销（Actor 被删除）
- `set_actor_transform`（通道 C / 通道 B）后可通过 Undo 恢复旧 Transform
- `import_assets` / `create_blueprint_child` 同理
- 可通过 `undo_last_transaction` 接口远程触发回滚（无需人工 Ctrl+Z）

**可利用此能力实现基础回滚**——mismatch 或验证失败时，通过 UE5 原生 Undo 撤销最近的写操作。

### 8.2 回滚策略

| 回滚层级 | 机制 | 状态 |
|---|---|---|
| 单步 Undo | C++ `FScopedTransaction`（通道 C）/ `generateTransaction`（通道 B） | ✅ 可用 |
| 多步 Undo | 连续调用 `GEditor->UndoTransaction()` | ✅ 可用 |
| 远程 Undo | 调用 `undo_last_transaction(steps)`（通道 C） | ✅ 可用 |
| 完整 Checkpoint / Rollback | VCS（Git/Perforce）级别回退 | 后续扩展 |
| 资产级回滚 | 通过 `get_dirty_assets` 识别 → VCS revert | 后续扩展 |

### 8.3 与验证闭环的结合

当验证层判定 `mismatch` 时，推荐处理流程：
1. 记录 mismatch 详情到报告
2. 若可用通道 C：调用 `undo_last_transaction` 自动回滚
3. 若通过通道 B 执行且 Transaction 生效：可用 UE5 原生 Undo 回滚
4. 如果操作通过通道 A 执行（无 Transaction）→ 需手动修正或使用补偿操作
5. 重新执行读回验证，确认 Undo 是否成功

> 完整的 UE5 Transaction System 和回滚能力说明，参见 `Docs/ue5_capability_map.md` 第 5.6 节。
