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
using Gauntlet;
using UnrealBuildTool;

namespace AgentBridge.Gauntlet
{
    internal static class AgentBridgeGauntletDefaults
    {
        // 全量测试：Project.AgentBridge 覆盖 L1/L2/L3.UITool；
        // Functional Test 地图树在 UE5.5.4 下不挂在 Project.AgentBridge 下，需要单独补一个前缀。
        public const string AllTestsFilter =
            "Project.AgentBridge+Project.Functional Tests.Tests.FTEST_WarehouseDemo";

        // 冒烟测试只跑 L1 + L2，避免无 GPU 节点被 UI 渲染依赖拖住。
        public const string SmokeTestsFilter =
            "Project.AgentBridge.L1+Project.AgentBridge.L2";
    }

    public abstract class AgentBridgeConfigBase : UnrealTestConfiguration
    {
        // 由派生类生成本轮 Editor 需要追加的 AgentBridge 参数。
        protected abstract string BuildAgentBridgeCommandLine();

        // 测试控制器类位于 AgentBridgeTests 插件内。
        // 在回退到“项目壳 + 子模块”结构后，不能再依赖 Target receipt 的默认启用副作用，
        // 否则普通打开项目不会报错，但 Gauntlet 的独立 Editor 会话也不会自动加载测试插件。
        // 这里改为每次 Gauntlet 会话都显式带上测试插件路径和启用参数，保证控制器类可发现。
        protected virtual string BuildAgentBridgeTestPluginCommandLine(UnrealAppConfig AppConfig)
        {
            if (AppConfig.ProjectFile == null)
            {
                throw new Exception("Gauntlet session is missing AppConfig.ProjectFile; cannot resolve AgentBridgeTests.uplugin path.");
            }

            string ProjectRoot = AppConfig.ProjectFile.Directory.FullName;
            string TestPluginFile = Path.Combine(
                ProjectRoot,
                "Plugins",
                "AgentBridge",
                "AgentBridgeTests",
                "AgentBridgeTests.uplugin");

            if (!File.Exists(TestPluginFile))
            {
                throw new FileNotFoundException(
                    $"AgentBridgeTests plugin descriptor not found: {TestPluginFile}",
                    TestPluginFile);
            }

            return $"-PLUGIN=\"{TestPluginFile}\" -EnablePlugins=AgentBridgeTests";
        }

        public override void ApplyToConfig(
            UnrealAppConfig AppConfig,
            UnrealSessionRole ConfigRole,
            IEnumerable<UnrealSessionRole> OtherRoles)
        {
            base.ApplyToConfig(AppConfig, ConfigRole, OtherRoles);

            if (AppConfig.ProcessType.IsEditor())
            {
                // UE5.5.4 的 Gauntlet 编辑器控制器不会在普通 Editor 启动阶段直接初始化，
                // 而是在 PreBeginPIE 回调里调用 PerformInitialization()。
                // 因此只要本轮会话依赖 -gauntlet=... 控制器，就必须显式带上 PIE，
                // 否则控制器类虽然出现在命令行里，但不会真正被实例化。
                AppConfig.CommandLineParams.Add("PIE");
                AppConfig.CommandLineParams.AddRawCommandline(BuildAgentBridgeTestPluginCommandLine(AppConfig));

                string ExtraArgs = BuildAgentBridgeCommandLine();
                if (!string.IsNullOrEmpty(ExtraArgs))
                {
                    AppConfig.CommandLineParams.AddRawCommandline(ExtraArgs);
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

        protected override string BuildAgentBridgeCommandLine()
        {
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
                $"-AgentBridgeFilter=\"{AgentBridgeGauntletDefaults.AllTestsFilter}\" " +
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

        protected override string BuildAgentBridgeCommandLine()
        {
            return
                // 这里显式补 -NullRHI，避免仅依赖基类 Nullrhi 属性在不同 RunUnreal 版本下的隐式拼接。
                // Task16 的 SmokeTests 目标就是“无 GPU 的 L1/L2 冒烟验证”，
                // 所以最终命令行里必须肉眼可见地带上 -NullRHI，便于日志审计。
                $"-NullRHI -NoSound -NoSplash " +
                $"-AgentBridgeFilter=\"{AgentBridgeGauntletDefaults.SmokeTestsFilter}\" " +
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
    ///   RunUAT.bat -ScriptsForProject=MyProject.uproject -ScriptDir=Gauntlet RunUnreal
    ///     -project=MyProject.uproject -build=editor -platform=Win64
    ///     -Namespaces=AgentBridge.Gauntlet -test=AllTests -unattended
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

