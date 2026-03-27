// AgentBridgeCommandlet.h
// AGENT + UE5 可操作层 - Commandlet 无头执行入口
//
// 角色：
// 1. 通过 -Spec 执行结构化 Spec（借助 Python Orchestrator）
// 2. 通过 -RunTests 触发自动化测试
// 3. 通过 -Tool 执行单工具（L1/L2）
//
// 退出码约定：
// 0 = success
// 1 = warning / mismatch
// 2 = failed / validation_error / 参数错误

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BridgeTypes.h"
#include "AgentBridgeCommandlet.generated.h"

UCLASS()
class AGENTBRIDGE_API UAgentBridgeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAgentBridgeCommandlet();
	virtual int32 Main(const FString& Params) override;

private:
	void ParseParams(const FString& Params);
	int32 RunSpec();
	int32 RunTests();
	int32 RunSingleTool();
	void WriteReport(const FString& JsonContent);

	// 参数读取辅助
	FString GetNamedParam(const TCHAR* Key) const;
	bool GetBoolNamedParam(const TCHAR* Key, bool bDefaultValue = false) const;
	bool TryGetVectorParam(const TCHAR* Key, FVector& OutVector) const;
	bool TryGetRotatorParam(const TCHAR* Key, FRotator& OutRotator) const;
	static bool ParseCsv3(const FString& Csv, double& X, double& Y, double& Z);
	static int32 StatusToExitCode(EBridgeStatus Status);

	// 主参数
	FString SpecPath;      // -Spec=xxx.yaml
	FString TestFilter;    // -RunTests=Project.AgentBridge.L1
	FString ToolName;      // -Tool=GetCurrentProjectState
	FString ReportPath;    // -Report=report.json

	// 原始参数与结果缓存
	FString RawParams;
	FString LastResultJson;
};
