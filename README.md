# AgentUE5Framework

> UE5 宿主项目可复用框架真源仓

## 1. 仓库定位

`AgentUE5Framework` 负责承接 **通用框架真源**，用于给任意 UE5 宿主项目提供：

- UE 编辑器插件真源
- Python Bridge / Orchestrator / Validators
- Schema 真源
- Spec 模板
- 通用文档、Gauntlet 配置、roadmap

本仓库不是具体游戏项目仓，不承载某个项目专属的：

- `.uproject`
- `Config/`
- `Content/`
- `Source/`
- 项目测试地图
- 项目实例 Spec
- 项目运行产物

## 2. 目录结构

```text
AgentUE5Framework/
├── UEPlugin/
│   ├── AgentBridge/
│   └── AgentBridgeTests/
├── PythonPackage/
│   └── agent_ue5/
│       ├── bridge/
│       ├── orchestrator/
│       ├── validators/
│       ├── schemas/
│       └── spec_templates/
├── Docs/
├── Gauntlet/
├── roadmap/
├── README.md
└── AGENTS.md
```

## 3. 与宿主项目的边界

推荐配套形态：

- 宿主项目仓保持标准 UE5 项目根
- 宿主项目仓中的 `Plugins/AgentBridge` / `Plugins/AgentBridgeTests` 作为安装态或镜像态
- 本仓库中的 `UEPlugin/AgentBridge` / `UEPlugin/AgentBridgeTests` 作为插件真源
- 宿主项目实例 Spec 放在宿主项目的 `AgentSpecs/`
- 项目配置放在宿主项目的 `AgentConfig/`
- 项目工具脚本放在宿主项目的 `AgentTools/`
- 运行产物放在宿主项目的 `Artifacts/`

## 4. 快速接入

1. 将 `UEPlugin/AgentBridge` 与 `UEPlugin/AgentBridgeTests` 安装到宿主项目的 `Plugins/`
2. 在宿主项目中保留标准 UE5 目录：`.uproject / Config / Content / Source / Plugins`
3. 将项目实例 Spec 放到宿主项目的 `AgentSpecs/`
4. 使用 `PythonPackage/agent_ue5/validators/start_ue_editor.ps1` 作为通用启动器
5. 使用 `PythonPackage/agent_ue5/validators/validate_examples.py` 做 schema/example 校验

## 5. 文档入口

- 框架架构：[Docs/architecture_overview.md](Docs/architecture_overview.md)
- 测试方案：[Docs/mvp_smoke_test_plan.md](Docs/mvp_smoke_test_plan.md)
- 工具契约：[Docs/tool_contract_v0_1.md](Docs/tool_contract_v0_1.md)
- 字段规范：[Docs/field_specification_v0_1.md](Docs/field_specification_v0_1.md)
- Spec 模板说明：[PythonPackage/agent_ue5/spec_templates/README.md](PythonPackage/agent_ue5/spec_templates/README.md)
