// AgentBridgeTests.Build.cs
// AgentBridgeTests 测试模块构建配置。

using UnrealBuildTool;

public class AgentBridgeTests : ModuleRules
{
    public AgentBridgeTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "AgentBridge",
            "UnrealEd",
            "EditorScriptingUtilities",
            "AutomationController",
            "AutomationDriver",
            "FunctionalTesting",
            "PythonScriptPlugin",
            "Json",
            "JsonUtilities",
            "Gauntlet",
        });
    }
}
