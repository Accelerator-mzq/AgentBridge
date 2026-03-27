// AgentBridge.Build.cs
// AGENT + UE5 可操作层 — C++ Editor Plugin 构建配置
//
// 本模块依赖的 UE5 官方模块：
//   Core / CoreUObject / Engine         — 引擎基础
//   UnrealEd                            — Editor 子系统 + Level/Asset Library
//   EditorScriptingUtilities            — UEditorLevelLibrary / UEditorAssetLibrary — L1
//   RemoteControl                       — Remote Control API（通道 B 端点暴露）— L1
//   PythonScriptPlugin                  — Python↔C++ 互调（通道 A）— L1
//   Json / JsonUtilities                — 统一 JSON 响应构造
//   HTTP                                — UAT 子进程 + 健康检查 — L2
//   AutomationController                — Automation Test 执行控制 — L2
//   AutomationDriver                    — UI 输入模拟（L3 UI 工具层执行后端）
//   ContentBrowser                      — 资产定位（L3 DragAssetToViewport）

using UnrealBuildTool;

public class AgentBridge : ModuleRules
{
    public AgentBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // ============================================================
        // 公开依赖（对依赖本模块的其他模块可见）
        // ============================================================
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "EditorSubsystem",
            "UnrealEd",
            "Json",
            "JsonUtilities",
        });

        // ============================================================
        // 私有依赖（仅本模块内部使用）
        // ============================================================
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // Editor 核心
            "EditorScriptingUtilities",

            // 资产工具（ImportAssetTasks / CreateAsset）
            "AssetTools",

            // Remote Control API（通道 B）
            "RemoteControl",

            // Python 互调（通道 A）
            "PythonScriptPlugin",

            // JSON 序列化
            "Serialization",

            // HTTP（UAT 健康检查 + Remote Control 客户端）
            "HTTP",

            // Automation 控制（触发 RunTests）— L2
            "AutomationController",

            // Automation Driver（UI 输入模拟）— L3
            "AutomationDriver",

            // Slate UI（Widget 查找 + L3 UI 操作）
            "Slate",
            "SlateCore",
            "EditorStyle",

            // LevelEditor（活跃 Viewport 获取 + 走官方 DropObjectsAtCoordinates 放置路径）
            "LevelEditor",

            // Content Browser（L3 DragAssetToViewport 资产定位）
            "ContentBrowser",

            // 输入输出
            "InputCore",
        });

        // ============================================================
        // 条件依赖
        // ============================================================

        // Gauntlet 支持（仅在有 Gauntlet 模块时启用）
        if (Target.bBuildWithEditorOnlyData)
        {
            PrivateDependencyModuleNames.Add("Gauntlet");
        }
    }
}
