// UATRunner.h
// AGENT + UE5 可操作层 - UAT 子进程封装

#pragma once

#include "CoreMinimal.h"

struct AGENTBRIDGE_API FUATRunResult
{
	// UAT 进程是否启动成功
	bool bLaunched = false;

	// 是否已完成（同步模式下应为 true，异步模式为 false）
	bool bCompleted = false;

	// UAT 退出码（同步模式有效）
	int32 ExitCode = -1;

	// 实际执行命令行
	FString CommandLine;

	// 标准输出（同步模式）
	FString StdOut;

	// 标准错误（同步模式）
	FString StdErr;

	// 启动失败时的错误描述
	FString ErrorMessage;

	bool IsSuccess() const { return bLaunched && bCompleted && ExitCode == 0; }
};

class AGENTBRIDGE_API FUATRunner
{
public:
	FUATRunner();

	// RunUAT.bat/sh 是否可用
	bool IsUATAvailable() const;

	// 编译 + 烹饪 + 打包
	FUATRunResult BuildCookRun(
		const FString& Platform = TEXT("Win64"),
		const FString& Configuration = TEXT("Development"),
		bool bSync = false);

	// 通过 UAT 运行自动化测试
	FUATRunResult RunAutomationTests(
		const FString& Filter,
		const FString& ReportPath = TEXT(""),
		bool bSync = false);

	// 启动 Gauntlet 会话
	FUATRunResult RunGauntlet(
		const FString& TestConfigName,
		bool bSync = false);

	// 执行任意 UAT 命令
	FUATRunResult RunCustomCommand(
		const FString& Command,
		bool bSync = false);

private:
	// 自动检测 RunUAT.bat/sh
	FString DetectRunUATPath() const;

	// 底层执行
	FUATRunResult ExecuteUAT(const FString& Args, bool bSync);

	FString RunUATPath;
};
