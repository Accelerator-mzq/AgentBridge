# AgentBridge Gauntlet

`Gauntlet/` 是 `AgentUE5Framework` 中的 Gauntlet 真源目录。

这里保留的是通用会话编排能力：

- `AgentBridge.TestConfig.cs`
- `AgentBridgeGauntlet.Automation.csproj`
- 项目配置模板

这里不再固化宿主项目的测试过滤器、Functional Test 路径或项目级默认值。
这些项目语义应由宿主项目通过配置文件注入，例如：

- `all_tests_filter`
- `smoke_tests_filter`
- `functional_test_prefixes`
- `spec_execution_max_duration`

宿主项目建议保留：

- `AgentConfig/gauntlet_project_config.json`
- `AgentTools/run_gauntlet.ps1`
- `Docs/project_gauntlet_integration.md`

推荐调用方式：

1. 宿主项目包装脚本定位 `AgentUE5Framework`
2. 包装脚本读取项目侧 `gauntlet_project_config.json`
3. 包装脚本通过 `RunUAT` + `-ScriptDir=<FrameworkRoot>\\Gauntlet` 调用本目录下的 Gauntlet 真源
4. `AgentBridge.TestConfig.cs` 通过 `-ProjectConfigPath=` 读取项目配置

当前约束：

- 插件安装态仍保留在宿主 UE5 项目仓
- `AgentBridgeGauntletController` 继续位于测试插件中
- 根级 `Gauntlet/` 从项目仓退场前，必须先验证项目包装层可稳定驱动本真源
