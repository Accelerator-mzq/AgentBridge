# 系统架构概述

> 目标引擎版本：UE5.5.4 | 文档版本：v0.3 | 适用范围：AgentUE5Framework

## 1. 核心定位

`AgentUE5Framework` 是位于 AI Agent 与 UE5 官方能力之间的**受控编排层**。

它不创造新的引擎能力，而是把 UE5 官方 API 统一收敛为结构化工具，并补上：

- 参数约束
- 写前查询
- 写后读回
- 独立验证
- 可回滚事务
- 审计与报告

## 2. 仓库边界

框架仓负责：

- `UEPlugin/`：插件真源
- `PythonPackage/agent_ue5/`：Python 包真源
- `Docs/`：通用文档
- `Gauntlet/`：CI/CD 编排配置
- `roadmap/`：路线图

宿主项目负责：

- `.uproject / Config / Content / Source / Plugins`
- 项目实例 Spec
- 项目地图、资产、测试记录
- 项目运行产物

## 3. 逻辑分层

```text
AI Agent
  -> Orchestrator
    -> Bridge
      -> UE5 官方 API
```

更细分后可看成：

```text
AI Agent
  -> 结构化 Spec
  -> 计划生成 / 执行 / 验证 / 报告
  -> 三层受控工具
     - L1 语义工具
     - L2 编辑器服务工具
     - L3 UI 工具
  -> UE5 官方模块
```

## 4. 三层受控工具

| 层次 | 角色 | 默认用途 |
|---|---|---|
| L1 | 语义工具 | 查询/写入 Actor、Asset、Level 状态 |
| L2 | 编辑器服务工具 | 构建、测试、截图、MapCheck、保存 |
| L3 | UI 工具 | 当 L1 无对应能力时，作为受约束补充路径 |

默认优先级：`L1 > L2 > L3`。

## 5. 三条执行通道

| 通道 | 方式 | 说明 |
|---|---|---|
| A | Python Editor Scripting | 进程内原型与脚本调用 |
| B | Remote Control API | Editor 外远程调用 |
| C | C++ Plugin | 推荐主干，提供统一接口、事务与验证 |

所有通道最终都落到同一套 UE5 官方 API。

## 6. 插件职责

### AgentBridge

- 提供查询、写入、验证、构建接口
- 统一响应格式
- 管理 `FScopedTransaction`
- 暴露 Commandlet 入口
- 暴露可被外部调用的接口

### AgentBridgeTests

- 注册自动化测试
- 承载 L1/L2/L3 测试入口
- 不承载业务真逻辑

## 7. Python 包职责

```text
agent_ue5/
├── bridge/         # 查询、写入、UI、RC 客户端、UAT 封装
├── orchestrator/   # Spec 读取、计划、执行、验证、报告
├── validators/     # schema/example 校验与通用启动器
├── schemas/        # Schema 真源
└── spec_templates/ # 模板
```

## 8. 宿主项目接入点

宿主项目只需要保持标准 UE5 项目根，并在项目侧补齐：

- `AgentSpecs/`：项目实例 Spec
- `AgentConfig/`：项目配置
- `AgentTools/`：项目包装脚本
- `Artifacts/`：项目产物

例如：

```text
<HostProject>/
├── <HostProject>.uproject
├── Config/
├── Content/
├── Source/
├── Plugins/
├── AgentSpecs/
├── AgentConfig/
├── AgentTools/
└── Artifacts/
```

## 9. 典型执行链

```text
Spec
  -> 查询当前状态
  -> 生成执行计划
  -> 调用受控工具
  -> 写后读回
  -> 独立验证
  -> 生成报告
```

命令与路径应使用占位表达，例如：

- `<HostProject>.uproject`
- `AgentSpecs/<domain>/<name>.yaml`
- `Artifacts/reports/<task>.json`
- `/Game/Tests/<FunctionalMap>`
