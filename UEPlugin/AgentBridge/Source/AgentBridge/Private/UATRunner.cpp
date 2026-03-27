// UATRunner.cpp
// AGENT + UE5 可操作层 - UAT 子进程封装实现

#include "UATRunner.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

FUATRunner::FUATRunner()
{
	RunUATPath = DetectRunUATPath();
}

FString FUATRunner::DetectRunUATPath() const
{
	const FString EngineDir = FPaths::EngineDir();

#if PLATFORM_WINDOWS
	FString Candidate = EngineDir / TEXT("Build/BatchFiles/RunUAT.bat");
#else
	FString Candidate = EngineDir / TEXT("Build/BatchFiles/RunUAT.sh");
#endif

	Candidate = FPaths::ConvertRelativePathToFull(Candidate);
	if (!FPaths::FileExists(Candidate))
	{
		return FString();
	}
	return Candidate;
}

bool FUATRunner::IsUATAvailable() const
{
	return !RunUATPath.IsEmpty() && FPaths::FileExists(RunUATPath);
}

FUATRunResult FUATRunner::ExecuteUAT(const FString& Args, const bool bSync)
{
	FUATRunResult Result;
	Result.CommandLine = FString::Printf(TEXT("\"%s\" %s"), *RunUATPath, *Args);

	if (!IsUATAvailable())
	{
		Result.ErrorMessage = FString::Printf(TEXT("RunUAT not found: %s"), *RunUATPath);
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge UAT] %s"), *Result.ErrorMessage);
		return Result;
	}

	uint32 ProcessId = 0;
	void* StdOutReadPipe = nullptr;
	void* StdOutWritePipe = nullptr;
	void* StdErrReadPipe = nullptr;
	void* StdErrWritePipe = nullptr;

	if (bSync)
	{
		FPlatformProcess::CreatePipe(StdOutReadPipe, StdOutWritePipe);
		FPlatformProcess::CreatePipe(StdErrReadPipe, StdErrWritePipe);
	}

	UE_LOG(LogTemp, Log, TEXT("[AgentBridge UAT] Launch: %s"), *Result.CommandLine);

	FProcHandle ProcHandle;
#if PLATFORM_WINDOWS
	ProcHandle = FPlatformProcess::CreateProc(
		*RunUATPath,
		*Args,
		/*bLaunchDetached=*/true,
		/*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/false,
		&ProcessId,
		/*PriorityModifier=*/0,
		nullptr,
		bSync ? StdOutWritePipe : nullptr,
		nullptr,
		bSync ? StdErrWritePipe : nullptr);
#else
	ProcHandle = FPlatformProcess::CreateProc(
		*RunUATPath,
		*Args,
		/*bLaunchDetached=*/true,
		/*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/false,
		&ProcessId,
		/*PriorityModifier=*/0,
		nullptr,
		bSync ? StdOutWritePipe : nullptr,
		nullptr);
#endif

	if (!ProcHandle.IsValid())
	{
		Result.ErrorMessage = FString::Printf(TEXT("Failed to launch UAT process: %s"), *RunUATPath);
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge UAT] %s"), *Result.ErrorMessage);
		if (StdOutReadPipe || StdOutWritePipe) FPlatformProcess::ClosePipe(StdOutReadPipe, StdOutWritePipe);
		if (StdErrReadPipe || StdErrWritePipe) FPlatformProcess::ClosePipe(StdErrReadPipe, StdErrWritePipe);
		return Result;
	}

	Result.bLaunched = true;

	if (!bSync)
	{
		Result.bCompleted = false;
		Result.ExitCode = -1;
		UE_LOG(LogTemp, Log, TEXT("[AgentBridge UAT] Process launched in async mode, PID=%u"), ProcessId);
		return Result;
	}

	// 同步模式：持续读管道，避免大输出导致管道阻塞。
	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		if (StdOutReadPipe)
		{
			Result.StdOut += FPlatformProcess::ReadPipe(StdOutReadPipe);
		}
		if (StdErrReadPipe)
		{
			Result.StdErr += FPlatformProcess::ReadPipe(StdErrReadPipe);
		}
		FPlatformProcess::Sleep(0.05f);
	}

	FPlatformProcess::WaitForProc(ProcHandle);

	if (StdOutReadPipe)
	{
		Result.StdOut += FPlatformProcess::ReadPipe(StdOutReadPipe);
	}
	if (StdErrReadPipe)
	{
		Result.StdErr += FPlatformProcess::ReadPipe(StdErrReadPipe);
	}

	int32 ReturnCode = -1;
	if (!FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode))
	{
		ReturnCode = -1;
	}

	Result.bCompleted = true;
	Result.ExitCode = ReturnCode;

	if (StdOutReadPipe || StdOutWritePipe) FPlatformProcess::ClosePipe(StdOutReadPipe, StdOutWritePipe);
	if (StdErrReadPipe || StdErrWritePipe) FPlatformProcess::ClosePipe(StdErrReadPipe, StdErrWritePipe);
	FPlatformProcess::CloseProc(ProcHandle);

	UE_LOG(LogTemp, Log, TEXT("[AgentBridge UAT] Sync finished, ExitCode=%d"), ReturnCode);
	return Result;
}

FUATRunResult FUATRunner::BuildCookRun(
	const FString& Platform,
	const FString& Configuration,
	const bool bSync)
{
	const FString ProjectPath = FPaths::GetProjectFilePath();
	const FString Args = FString::Printf(
		TEXT("BuildCookRun -project=\"%s\" -platform=%s -clientconfig=%s -cook -stage -pak"),
		*ProjectPath,
		*Platform,
		*Configuration);
	return ExecuteUAT(Args, bSync);
}

FUATRunResult FUATRunner::RunAutomationTests(
	const FString& Filter,
	const FString& ReportPath,
	const bool bSync)
{
	const FString ProjectPath = FPaths::GetProjectFilePath();
	// UE5.5 不存在独立的 RunAutomationTests 子命令，需通过 BuildCookRun 的 editortest 路径触发。
	FString Args = FString::Printf(
		TEXT("BuildCookRun -project=\"%s\" -run -editortest -unattended -nullrhi -NoP4"),
		*ProjectPath);

	if (!Filter.IsEmpty())
	{
		Args += FString::Printf(TEXT(" -RunAutomationTest=%s"), *Filter);
	}
	else
	{
		Args += TEXT(" -RunAutomationTests");
	}

	// BuildCookRun 没有稳定的 -ReportOutputPath 参数入口，暂保留字段用于上层记录。
	if (!ReportPath.IsEmpty())
	{
		Args += FString::Printf(TEXT(" -addcmdline=\"-ReportExportPath=\\\"%s\\\"\""), *ReportPath);
	}

	return ExecuteUAT(Args, bSync);
}

FUATRunResult FUATRunner::RunGauntlet(
	const FString& TestConfigName,
	const bool bSync)
{
	const FString ProjectPath = FPaths::GetProjectFilePath();
	const FString Args = FString::Printf(
		TEXT("RunGauntlet -project=\"%s\" -Test=%s -unattended"),
		*ProjectPath,
		*TestConfigName);
	return ExecuteUAT(Args, bSync);
}

FUATRunResult FUATRunner::RunCustomCommand(
	const FString& Command,
	const bool bSync)
{
	FString Args = Command.TrimStartAndEnd();
	if (Args.IsEmpty())
	{
		FUATRunResult Result;
		Result.ErrorMessage = TEXT("RunCustomCommand requires non-empty command");
		return Result;
	}

	if (!Args.Contains(TEXT("-project="), ESearchCase::IgnoreCase))
	{
		Args += FString::Printf(TEXT(" -project=\"%s\""), *FPaths::GetProjectFilePath());
	}

	return ExecuteUAT(Args, bSync);
}
