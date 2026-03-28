// AgentBridgeModule.cpp
// AGENT + UE5 可操作层 — AgentBridge Plugin 模块注册
//
// 本模块在 Editor 启动时自动加载（LoadingPhase: PostEngineInit）。
// UAgentBridgeSubsystem 作为 UEditorSubsystem 由引擎自动实例化，
// 无需在此处手动创建。
//
// 本文件的职责：
//   1. 注册/注销模块
//   2. 输出启动日志（便于确认 Plugin 是否正确加载）
//   3. 未来扩展点：注册 Remote Control Preset / 自定义 Console Command

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"

#define LOCTEXT_NAMESPACE "FAgentBridgeModule"

class FAgentBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogTemp, Log, TEXT("[AgentBridge] Plugin loaded, version 0.3.0"));
		UE_LOG(LogTemp, Log,
			TEXT("[AgentBridge] Access via: GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>()"));
		UE_LOG(LogTemp, Log,
			TEXT("[AgentBridge] Remote Control: BlueprintCallable functions auto-exposed on :30010"));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogTemp, Log, TEXT("[AgentBridge] Plugin unloaded."));
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAgentBridgeModule, AgentBridge)

