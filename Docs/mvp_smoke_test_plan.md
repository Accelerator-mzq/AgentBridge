# 测试方案

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AgentUE5Framework

## 1. 文档目的

本文档定义框架级的 **MVP 冒烟测试模板**，用于验证：

- 工具接口可调用
- 写后读回成立
- 验证链条闭环
- 宿主项目已正确安装插件和测试入口

本文档是框架模板，不记录某个具体项目的历史任务结果。

## 2. 三层测试体系

| 层次 | 方式 | 目标 |
|---|---|---|
| L1 | Simple Automation Test | 单接口正确性 |
| L2 | Automation Spec | 多接口闭环验证 |
| L3 | Functional Testing | 宿主项目集成验证 |

## 3. 前置条件

| 条件 | 要求 |
|---|---|
| 宿主项目 | 已安装 `AgentBridge` 和 `AgentBridgeTests` |
| UE5 Editor | 可正常打开宿主项目 |
| RC API | 已启用且服务可访问 |
| Schema 校验 | `validate_examples.py --strict` 通过 |
| Functional Map | 宿主项目提供一个 `/Game/Tests/<FunctionalMap>` |

## 4. L1 冒烟范围

建议至少覆盖：

- `get_current_project_state`
- `list_level_actors`
- `get_actor_state`
- `get_actor_bounds`
- `get_asset_metadata`
- `get_dirty_assets`
- `run_map_check`
- `spawn_actor`
- `set_actor_transform`
- `import_assets`
- `create_blueprint_child`

如果宿主项目启用了 UI 工具，还应补：

- `is_automation_driver_available`
- `click_detail_panel_button`
- `type_in_detail_panel_field`
- `drag_asset_to_viewport`

## 5. L2 闭环范围

建议至少覆盖三类闭环：

1. `spawn -> readback`
2. `modify -> readback -> undo`
3. `ui_tool -> l1_verify`

容差建议：

| 字段 | 建议容差 |
|---|---|
| location | ≤ 0.01 cm |
| rotation | ≤ 0.01 degrees |
| relative_scale3d | ≤ 0.001 |
| L3 UI location | ≤ 100 cm |

## 6. L3 宿主项目接入模板

框架只要求宿主项目提供：

- 一张 functional test 地图
- 一个 `AFunctionalTest` 入口
- 可选的项目实例 Spec

推荐占位写法：

```text
/Game/Tests/<FunctionalMap>
AgentSpecs/<domain>/<scenario>.yaml
Artifacts/reports/<task>.json
```

## 7. 执行方式

| 方式 | 用途 |
|---|---|
| Session Frontend | 本地开发调试 |
| Console `Automation RunTests` | 快速执行 L1/L2 |
| Commandlet | 无头执行 |
| UAT | CI/CD |
| Gauntlet | 完整会话编排 |

## 8. 冒烟记录模板

```yaml
test_record:
  date: "YYYY-MM-DD"
  engine_version: "5.5.4"
  host_project: "<HostProject>"
  test_level: "/Game/Tests/<FunctionalMap>"
  plugin_version: "0.3.0"
  execution_method: "session_frontend"

  l1_results: {}
  l2_results: {}
  l3_results: {}

  schema_validation:
    validate_examples_strict: PASS

  notes: ""
```

## 9. 文档边界

下面这些内容不应继续写进框架版测试方案：

- 具体项目地图名
- 具体任务号
- 某次测试执行日志
- 某个项目的最终验收记录

这类内容应放在宿主项目仓自己的测试记录文档里。
