// AgentBridge.TestConfig.cs
// AGENT + UE5 可操作层 — Gauntlet C# 测试会话配置（UE5.5.4）
//
// 说明：
// 1. RunUnreal 入口需要解析 ITestNode，而不是直接解析 UnrealTestConfiguration。
// 2. 因此这里采用“Node + Config”双层结构：
//    - Node 决定“测试会话怎么启动”
//    - Config 决定“Editor 进程带哪些参数”
// 3. Editor 内真正执行测试的逻辑，仍由 AgentBridgeGauntletController 负责。

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using Gauntlet;
using UnrealBuildTool;

namespace AgentBridge.Gauntlet
{
    public sealed class ProjectGauntletConfig
    {
        [JsonPropertyName("all_tests_filter")]
        public string AllTestsFilter { get; set; } = string.Empty;

        [JsonPropertyName("smoke_tests_filter")]
        public string SmokeTestsFilter { get; set; } = string.Empty;

        [JsonPropertyName("all_tests_max_duration")]
        public int? AllTestsMaxDuration { get; set; }

        [JsonPropertyName("smoke_tests_max_duration")]
        public int? SmokeTestsMaxDuration { get; set; }

        [JsonPropertyName("spec_execution_max_duration")]
        public int? SpecExecutionMaxDuration { get; set; }

        [JsonPropertyName("functional_test_prefixes")]
        public List<string> FunctionalTestPrefixes { get; set; } = new();
    }

    public abstract class AgentBridgeConfigBase : UnrealTestConfiguration
    {
        private static readonly JsonSerializerOptions JsonOptions = new()
        {
            PropertyNameCaseInsensitive = true
        };

        private ProjectGauntletConfig CachedProjectConfig;

        [AutoParam("")]
        public string ProjectConfigPath { get; set; } = string.Empty;

        // 由派生类生成本轮 Editor 需要追加的 AgentBridge 参数。
        protected abstract string BuildAgentBridgeCommandLine();

        // 派生类可以覆盖默认超时，但项目配置优先。
        protected virtual int? ResolveConfiguredMaxDuration(ProjectGauntletConfig projectConfig)
        {
            return null;
        }

        protected ProjectGauntletConfig GetProjectGauntletConfig()
        {
            if (CachedProjectConfig != null)
            {
                return CachedProjectConfig;
            }

            string configPath = ResolveProjectConfigPath();
            if (!File.Exists(configPath))
            {
                throw new Exception(
                    $"AgentBridge Gauntlet project config not found: {configPath}. " +
                    "Please pass -ProjectConfigPath=<path> or set AGENT_UE5_GAUNTLET_PROJECT_CONFIG.");
            }

            string json = File.ReadAllText(configPath);
            CachedProjectConfig = JsonSerializer.Deserialize<ProjectGauntletConfig>(json, JsonOptions)
                ?? throw new Exception($"Failed to parse project config: {configPath}");
            return CachedProjectConfig;
        }

        protected string RequireProjectFilter(Func<ProjectGauntletConfig, string> selector, string fieldName)
        {
            string value = selector(GetProjectGauntletConfig())?.Trim() ?? string.Empty;
            if (string.IsNullOrEmpty(value))
            {
                throw new Exception($"Project config field '{fieldName}' is required.");
            }

            return value;
        }

        private string ResolveProjectConfigPath()
        {
            if (!string.IsNullOrWhiteSpace(ProjectConfigPath))
            {
                return ProjectConfigPath;
            }

            string envPath = Environment.GetEnvironmentVariable("AGENT_UE5_GAUNTLET_PROJECT_CONFIG");
            if (!string.IsNullOrWhiteSpace(envPath))
            {
                return envPath;
            }

            throw new Exception(
                "Project Gauntlet config path is required. " +
                "Pass -ProjectConfigPath=<path> or set AGENT_UE5_GAUNTLET_PROJECT_CONFIG.");
        }

        public override void ApplyToConfig(
            UnrealAppConfig AppConfig,
            UnrealSessionRole ConfigRole,
            IEnumerable<UnrealSessionRole> OtherRoles)
        {
            base.ApplyToConfig(AppConfig, ConfigRole, OtherRoles);

            if (AppConfig.ProcessType.IsEditor())
            {
                ProjectGauntletConfig projectConfig = GetProjectGauntletConfig();
                int? configuredMaxDuration = ResolveConfiguredMaxDuration(projectConfig);
                if (configuredMaxDuration.HasValue)
                {
                    MaxDuration = configuredMaxDuration.Value;
                }

                // UE5.5.4 的 Gauntlet 编辑器控制器不会在普通 Editor 启动阶段直接初始化，
                // 而是在 PreBeginPIE 回调里调用 PerformInitialization()。
                // 因此只要本轮会话依赖 -gauntlet=... 控制器，就必须显式带上 PIE，
                // 否则控制器类虽然出现在命令行里，但不会真正被实例化。
                AppConfig.CommandLineParams.Add("PIE");

                string ExtraArgs = BuildAgentBridgeCommandLine();
                if (!string.IsNullOrEmpty(ExtraArgs))
                {
                    AppConfig.CommandLine += " " + ExtraArgs;
                }
            }
        }
    }

    public abstract class AgentBridgeTestNode<TConfigClass> : UnrealTestNode<TConfigClass>
        where TConfigClass : AgentBridgeConfigBase, new()
    {
        protected AgentBridgeTestNode(UnrealTestContext InContext)
            : base(InContext)
        {
        }

        public override TConfigClass GetConfiguration()
        {
            TConfigClass Config = base.GetConfiguration();
            UnrealTestRole EditorRole = Config.RequireRole(UnrealTargetRole.Editor);

            if (!EditorRole.Controllers.Contains("AgentBridgeGauntletController"))
            {
                EditorRole.Controllers.Add("AgentBridgeGauntletController");
            }

            return Config;
        }
    }

    public class AllTestsConfig : AgentBridgeConfigBase
    {
        public AllTestsConfig()
        {
            MaxDuration = 900;
        }

        protected override int? ResolveConfiguredMaxDuration(ProjectGauntletConfig projectConfig)
        {
            return projectConfig.AllTestsMaxDuration;
        }

        protected override string BuildAgentBridgeCommandLine()
        {
            string allTestsFilter = RequireProjectFilter(
                config => config.AllTestsFilter,
                "all_tests_filter");

            return
                // AllTests 需要保留真实渲染，不能像 Smoke 那样直接走 -NullRHI。
                // 但 UE5.5.4 的无人值守 Gauntlet Editor 会话在进入 PIE 前，会先经过
                // FWaitForInteractiveFrameRate(DefaultInteractiveFramerate=5 by BaseEngine.ini)。
                // 在当前 CI/本机会话里，这个等待容易长期停在 1 FPS，导致控制器还没开始发现测试就先被超时杀掉。
                // 因此这里仅对本轮 AllTests 会话下调 AutomationTestSettings 的启动门槛：
                //   - 1 FPS 即视为“可交互”
                //   - 连续维持 1 秒即可通过
                //   - 最多等待 30 秒
                // 这样保留真实渲染路径，同时避免卡死在引擎默认的交互帧率门前。
                $"-NoSound -NoSplash " +
                $"-ini:Engine:[/Script/Engine.AutomationTestSettings]:DefaultInteractiveFramerate=1," +
                $"[/Script/Engine.AutomationTestSettings]:DefaultInteractiveFramerateDuration=1," +
                $"[/Script/Engine.AutomationTestSettings]:DefaultInteractiveFramerateWaitTime=30 " +
                $"-AgentBridgeFilter=\"{allTestsFilter}\" " +
                $"-AgentBridgeMaxWait={MaxDuration}";
        }
    }

    public class SmokeTestsConfig : AgentBridgeConfigBase
    {
        public SmokeTestsConfig()
        {
            MaxDuration = 300;
            Nullrhi = true;
        }

        protected override int? ResolveConfiguredMaxDuration(ProjectGauntletConfig projectConfig)
        {
            return projectConfig.SmokeTestsMaxDuration;
        }

        protected override string BuildAgentBridgeCommandLine()
        {
            string smokeTestsFilter = RequireProjectFilter(
                config => config.SmokeTestsFilter,
                "smoke_tests_filter");

            return
                // 这里显式补 -NullRHI，避免仅依赖基类 Nullrhi 属性在不同 RunUnreal 版本下的隐式拼接。
                // Task16 的 SmokeTests 目标就是“无 GPU 的 L1/L2 冒烟验证”，
                // 所以最终命令行里必须肉眼可见地带上 -NullRHI，便于日志审计。
                $"-NullRHI -NoSound -NoSplash " +
                $"-AgentBridgeFilter=\"{smokeTestsFilter}\" " +
                $"-AgentBridgeMaxWait={MaxDuration}";
        }
    }

    public class SpecExecutionConfig : AgentBridgeConfigBase
    {
        [AutoParam("")]
        public string SpecPath { get; set; }

        public SpecExecutionConfig()
        {
            MaxDuration = 300;
        }

        protected override int? ResolveConfiguredMaxDuration(ProjectGauntletConfig projectConfig)
        {
            return projectConfig.SpecExecutionMaxDuration;
        }

        protected override string BuildAgentBridgeCommandLine()
        {
            if (string.IsNullOrEmpty(SpecPath))
            {
                throw new Exception("SpecExecution requires -SpecPath=<path>");
            }

            return
                $"-NoSound -NoSplash " +
                $"-AgentBridgeSpec=\"{SpecPath}\" " +
                $"-AgentBridgeMaxWait={MaxDuration}";
        }
    }

    /// <summary>
    /// 全量测试节点：L1 + L2 + L3.UITool + Functional Test。
    /// 调用方式：
    ///   RunUAT.bat -ScriptsForProject=MyProject.uproject -ScriptDir=<FrameworkRoot>\Gauntlet RunUnreal
    ///     -project=MyProject.uproject -build=editor -platform=Win64
    ///     -Namespaces=AgentBridge.Gauntlet -test=AllTests -ProjectConfigPath=<ProjectConfig> -unattended
    /// </summary>
    public class AllTests : AgentBridgeTestNode<AllTestsConfig>
    {
        public AllTests(UnrealTestContext InContext)
            : base(InContext)
        {
        }
    }

    /// <summary>
    /// 冒烟测试节点：仅 L1 + L2，可在 -NullRHI 下运行。
    /// </summary>
    public class SmokeTests : AgentBridgeTestNode<SmokeTestsConfig>
    {
        public SmokeTests(UnrealTestContext InContext)
            : base(InContext)
        {
        }
    }

    /// <summary>
    /// Spec 执行节点：通过 AgentBridgeGauntletController 转交给 AgentBridge Commandlet。
    /// </summary>
    public class SpecExecution : AgentBridgeTestNode<SpecExecutionConfig>
    {
        public SpecExecution(UnrealTestContext InContext)
            : base(InContext)
        {
        }
    }
}

namespace UnrealEditor
{
    using Gauntlet;

    // RunUnreal 在 UE5.5.4 中会把 Namespaces 重置为默认值，
    // 因此这里补一层 UnrealEditor 包装类型，让 -test=SmokeTests 这类短名仍可直接发现。
    public class AllTests : AgentBridge.Gauntlet.AllTests
    {
        public AllTests(UnrealTestContext InContext)
            : base(InContext)
        {
        }
    }

    public class SmokeTests : AgentBridge.Gauntlet.SmokeTests
    {
        public SmokeTests(UnrealTestContext InContext)
            : base(InContext)
        {
        }
    }

    public class SpecExecution : AgentBridge.Gauntlet.SpecExecution
    {
        public SpecExecution(UnrealTestContext InContext)
            : base(InContext)
        {
        }
    }
}
