// AgentBridgeGauntletController.h
// AGENT + UE5 可操作层 — Gauntlet 引擎内控制器
//
// UE5 官方模块：Gauntlet
// 基类：UGauntletTestController
//
// GauntletTestController 是 Gauntlet 在引擎进程内部的"触角"：
//   - C# 端（TestConfig）负责启动/监控/停止 Editor 进程
//   - C++ 端（TestController）负责在 Editor 内驱动测试执行
//
// 本 Controller 的职责：
//   1. OnInit：解析命令行参数（测试过滤 / Spec 路径）
//   2. OnTick：触发 Automation RunTests 并轮询完成状态
//   3. OnComplete：收集结果，设置退出码
//
// 工作流：
//   Gauntlet C# → 启动 Editor(-ExecCmds="GauntletController AgentBridgeGauntletController")
//     → Editor 实例化本 Controller
//     → OnInit() → OnTick() 轮询 → 测试完成 → EndTest(ExitCode)
//     → Gauntlet C# 收集退出码和日志

#pragma once

#include "CoreMinimal.h"
#include "GauntletTestController.h"
#include "AgentBridgeGauntletController.generated.h"

class IAutomationControllerManager;

/** Gauntlet 控制器内部状态机。 */
enum class EAgentBridgeGauntletState : uint8
{
	Initializing,
	WaitingForTestDiscovery,
	WaitingForAutomationRun,
	WaitingForSpecProcess,
	Finished
};

UCLASS()
class UAgentBridgeGauntletController : public UGauntletTestController
{
	GENERATED_BODY()

public:
	// ============================================================
	// UGauntletTestController 接口
	// ============================================================

	virtual void OnInit() override;
	virtual void OnTick(float TimeDelta) override;

protected:
	/** 结束测试并设置退出码 */
	void FinishWithExitCode(int32 ExitCode, const FString& Reason);

	/** 启动自动化测试执行。 */
	void StartAutomationRun();

	/** 汇总自动化测试结果并返回退出码。 */
	int32 BuildAutomationExitCode(FString& OutSummary) const;

	/** 启动 Spec Commandlet 子进程。 */
	bool LaunchSpecProcess(FString& OutError);

	/** 处理测试发现回调。 */
	void HandleTestsRefreshed();

private:
	/** 测试过滤表达式（从命令行 -AgentBridgeFilter 获取） */
	FString TestFilter;

	/** Spec 路径（从命令行 -AgentBridgeSpec 获取） */
	FString SpecPath;

	/** Spec 子进程报告路径（便于审计）。 */
	FString SpecReportPath;

	/** 等待计时器（秒） */
	float WaitTimer = 0.0f;

	/** 最大等待时间（秒） */
	float MaxWaitTime = 300.0f;

	/** 轮询间隔（秒） */
	float PollInterval = 1.0f;

	/** 轮询累计器 */
	float PollAccumulator = 0.0f;

	/** 当前状态机阶段。 */
	EAgentBridgeGauntletState CurrentState = EAgentBridgeGauntletState::Initializing;

	/** AutomationController 管理器。 */
	TSharedPtr<IAutomationControllerManager> AutomationController;

	/** 当前 Editor 会话 SessionId。 */
	FGuid SessionId;

	/** 发现测试列表回调句柄。 */
	FDelegateHandle TestsRefreshedHandle;

	/** 是否已经请求过测试列表。 */
	bool bRequestedTests = false;

	/** 是否已收到测试列表刷新回调。 */
	bool bTestsRefreshed = false;

	/** 本轮实际启用的测试名列表。 */
	TArray<FString> SelectedTests;

	/** Spec Commandlet 子进程句柄。 */
	FProcHandle SpecProcessHandle;
};

