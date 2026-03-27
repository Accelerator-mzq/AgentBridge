# AGENTS.md

## 1. 仓库角色

本仓库是 `AgentUE5Framework` 的**框架真源仓**，不是具体游戏项目仓。

这里应当保留的内容：

- `UEPlugin/` 下的插件真源
- `PythonPackage/agent_ue5/` 下的 Python 包真源
- `Docs/` 下的通用框架文档
- `Gauntlet/` 与 `roadmap/`
- `spec_templates/` 与 `schemas/`

这里不应长期保留的内容：

- 宿主项目的 `.uproject / Config / Content / Source`
- 项目实例 Spec
- 项目运行日志、截图、报告
- 某个项目专属地图或任务记录

## 2. 目录归属规则

- `UEPlugin/AgentBridge`、`UEPlugin/AgentBridgeTests`：插件真源
- `PythonPackage/agent_ue5/bridge`：通用桥接逻辑
- `PythonPackage/agent_ue5/orchestrator`：通用编排逻辑
- `PythonPackage/agent_ue5/validators`：通用校验和启动工具
- `PythonPackage/agent_ue5/schemas`：Schema 真源
- `PythonPackage/agent_ue5/spec_templates`：模板，不放项目实例

## 3. 修改原则

- 优先保持“宿主项目无关”
- 示例路径使用占位表达，不写死具体项目名、地图名、任务号
- Python 包内优先使用包内导入，避免继续依赖旧的脚本式相对路径
- PowerShell 文本读写显式使用 UTF-8
- 代码注释使用中文，注释只解释必要上下文

## 4. 与宿主项目的协作边界

- 宿主项目仓负责项目 README、项目 AGENTS、项目实例 Spec、项目运行产物
- 本仓库负责框架 README、框架 AGENTS、模板、通用启动器、通用测试方案
- 若某项内容同时带“框架模板”和“项目实例”两种身份，应优先拆开，不要继续混放在同一文件
