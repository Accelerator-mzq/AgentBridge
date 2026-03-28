// AgentBridgeGauntletController.cpp
// AGENT + UE5 可操作层 — Gauntlet 引擎内控制器实现

#include "AgentBridgeGauntletController.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IAutomationControllerModule.h"
#include "IAutomationReport.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogAgentBridgeGauntlet, Log, All);

namespace
{
	/** 将过滤表达式拆成 token 列表，支持 + / ; / , 三种分隔符。 */
	static TArray<FString> ParseFilterTokens(const FString& FilterExpression)
	{
		FString Normalized = FilterExpression;
		Normalized.ReplaceInline(TEXT(";"), TEXT("+"));
		Normalized.ReplaceInline(TEXT(","), TEXT("+"));

		TArray<FString> Tokens;
		Normalized.ParseIntoArray(Tokens, TEXT("+"), true);

		for (FString& Token : Tokens)
		{
			Token.TrimStartAndEndInline();
		}

		Tokens.RemoveAll([](const FString& Item)
		{
			return Item.IsEmpty();
		});

		return Tokens;
	}

	/** 测试名是否匹配任一过滤 token。 */
	static bool MatchesAnyFilterToken(const FString& TestName, const TArray<FString>& FilterTokens)
	{
		if (FilterTokens.Num() == 0)
		{
			return true;
		}

		for (const FString& Token : FilterTokens)
		{
			if (TestName.Equals(Token, ESearchCase::IgnoreCase)
				|| TestName.StartsWith(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	/** 尝试将当前 Editor 可执行路径切换成 Commandlet 版本。 */
	static FString ResolveCommandletExecutablePath()
	{
		FString ExecutablePath = FPlatformProcess::ExecutablePath();
		const FString ExecutableName = FPaths::GetCleanFilename(ExecutablePath);

		if (ExecutableName.Equals(TEXT("UnrealEditor.exe"), ESearchCase::IgnoreCase))
		{
			const FString Candidate = FPaths::Combine(FPaths::GetPath(ExecutablePath), TEXT("UnrealEditor-Cmd.exe"));
			if (FPaths::FileExists(Candidate))
			{
				return Candidate;
			}
		}

		return ExecutablePath;
	}
}

void UAgentBridgeGauntletController::OnInit()
{
	Super::OnInit();

	const FString CmdLine = FCommandLine::Get();
	FParse::Value(*CmdLine, TEXT("-AgentBridgeFilter="), TestFilter);
	FParse::Value(*CmdLine, TEXT("-AgentBridgeSpec="), SpecPath);

	TestFilter.TrimQuotesInline();
	SpecPath.TrimQuotesInline();

	if (TestFilter.IsEmpty() && SpecPath.IsEmpty())
	{
		// 默认仍保持 Project.AgentBridge，避免手工调试时无参直接跑空。
		TestFilter = TEXT("Project.AgentBridge");
	}

	FString MaxWaitString;
	if (FParse::Value(*CmdLine, TEXT("-AgentBridgeMaxWait="), MaxWaitString))
	{
		MaxWaitTime = FMath::Max(30.0f, FCString::Atof(*MaxWaitString));
	}

	WaitTimer = 0.0f;
	PollAccumulator = PollInterval;

	if (!SpecPath.IsEmpty())
	{
		CurrentState = EAgentBridgeGauntletState::WaitingForSpecProcess;
	}
	else
	{
		IAutomationControllerModule& AutomationControllerModule = IAutomationControllerModule::Get();
		AutomationController = AutomationControllerModule.GetAutomationController();
		AutomationController->Init();
		AutomationController->SetNumPasses(1);

		TestsRefreshedHandle = AutomationController->OnTestsRefreshed().AddUObject(this, &UAgentBridgeGauntletController::HandleTestsRefreshed);

		SessionId = FApp::GetSessionId();
		CurrentState = EAgentBridgeGauntletState::Initializing;
	}

	UE_LOG(
		LogAgentBridgeGauntlet,
		Log,
		TEXT("[AgentBridge Gauntlet] Initialized. Filter=%s, Spec=%s, MaxWait=%.0f, State=%d"),
		TestFilter.IsEmpty() ? TEXT("(none)") : *TestFilter,
		SpecPath.IsEmpty() ? TEXT("(none)") : *SpecPath,
		MaxWaitTime,
		static_cast<int32>(CurrentState));
}

void UAgentBridgeGauntletController::OnTick(float TimeDelta)
{
	Super::OnTick(TimeDelta);

	if (CurrentState == EAgentBridgeGauntletState::Finished)
	{
		return;
	}

	if (AutomationController.IsValid())
	{
		AutomationController->Tick();
	}

	WaitTimer += TimeDelta;
	PollAccumulator += TimeDelta;

	switch (CurrentState)
	{
	case EAgentBridgeGauntletState::Initializing:
	{
		if (!AutomationController.IsValid())
		{
			FinishWithExitCode(2, TEXT("AutomationController unavailable"));
			return;
		}

		if (AutomationController->IsReadyForTests())
		{
			WaitTimer = 0.0f;
			PollAccumulator = PollInterval;
			CurrentState = EAgentBridgeGauntletState::WaitingForTestDiscovery;
			UE_LOG(LogAgentBridgeGauntlet, Log, TEXT("[AgentBridge Gauntlet] AutomationController ready, begin worker discovery."));
		}
		else if (WaitTimer > MaxWaitTime)
		{
			FinishWithExitCode(2, FString::Printf(TEXT("AutomationController not ready after %.0f seconds"), WaitTimer));
			return;
		}
		break;
	}

	case EAgentBridgeGauntletState::WaitingForTestDiscovery:
	{
		if (!AutomationController.IsValid())
		{
			FinishWithExitCode(2, TEXT("AutomationController unavailable during discovery"));
			return;
		}

		if (bTestsRefreshed)
		{
			StartAutomationRun();
			return;
		}

		if (WaitTimer > MaxWaitTime)
		{
			FinishWithExitCode(2, FString::Printf(TEXT("Timed out waiting for test discovery after %.0f seconds"), WaitTimer));
			return;
		}

		if (PollAccumulator >= PollInterval)
		{
			PollAccumulator = 0.0f;

			if (AutomationController->GetNumDeviceClusters() == 0)
			{
				AutomationController->RequestAvailableWorkers(SessionId);
				UE_LOG(LogAgentBridgeGauntlet, Log, TEXT("[AgentBridge Gauntlet] RequestAvailableWorkers(SessionId=%s)"), *SessionId.ToString());
			}
			else if (!bRequestedTests)
			{
				AutomationController->RequestTests();
				bRequestedTests = true;
				UE_LOG(LogAgentBridgeGauntlet, Log, TEXT("[AgentBridge Gauntlet] RequestTests()"));
			}
		}
		break;
	}

	case EAgentBridgeGauntletState::WaitingForAutomationRun:
	{
		if (!AutomationController.IsValid())
		{
			FinishWithExitCode(2, TEXT("AutomationController unavailable during execution"));
			return;
		}

		if (WaitTimer > MaxWaitTime)
		{
			AutomationController->StopTests();
			FinishWithExitCode(2, FString::Printf(TEXT("Timed out waiting for tests after %.0f seconds"), WaitTimer));
			return;
		}

		const bool bFinishedByState = (AutomationController->GetTestState() != EAutomationControllerModuleState::Running)
			&& AutomationController->CheckTestResultsAvailable();
		if (bFinishedByState)
		{
			FString Summary;
			const int32 ExitCode = BuildAutomationExitCode(Summary);
			FinishWithExitCode(ExitCode, Summary);
			return;
		}
		break;
	}

	case EAgentBridgeGauntletState::WaitingForSpecProcess:
	{
		if (!SpecProcessHandle.IsValid())
		{
			FString LaunchError;
			if (!LaunchSpecProcess(LaunchError))
			{
				FinishWithExitCode(2, LaunchError);
				return;
			}

			WaitTimer = 0.0f;
			PollAccumulator = 0.0f;
			return;
		}

		if (!FPlatformProcess::IsProcRunning(SpecProcessHandle))
		{
			int32 SpecExitCode = 2;
			FPlatformProcess::GetProcReturnCode(SpecProcessHandle, &SpecExitCode);
			FPlatformProcess::CloseProc(SpecProcessHandle);
			SpecProcessHandle.Reset();

			FinishWithExitCode(
				SpecExitCode,
				FString::Printf(TEXT("Spec commandlet finished with exit code %d | report=%s"), SpecExitCode, *SpecReportPath));
			return;
		}

		if (WaitTimer > MaxWaitTime)
		{
			FPlatformProcess::TerminateProc(SpecProcessHandle, true);
			FPlatformProcess::CloseProc(SpecProcessHandle);
			SpecProcessHandle.Reset();
			FinishWithExitCode(2, FString::Printf(TEXT("Timed out waiting for spec commandlet after %.0f seconds"), WaitTimer));
			return;
		}
		break;
	}

	case EAgentBridgeGauntletState::Finished:
	default:
		break;
	}
}

void UAgentBridgeGauntletController::StartAutomationRun()
{
	if (!AutomationController.IsValid())
	{
		FinishWithExitCode(2, TEXT("AutomationController unavailable when starting tests"));
		return;
	}

	SelectedTests.Reset();

	// 通过空过滤器取回当前 Editor 会话里可见的全部测试，再做 AgentBridge 自己的精确筛选。
	TSharedPtr<AutomationFilterCollection> Filters = MakeShared<AutomationFilterCollection>();
	AutomationController->SetFilter(Filters);
	AutomationController->SetVisibleTestsEnabled(true);

	TArray<FString> AllVisibleTests;
	AutomationController->GetEnabledTestNames(AllVisibleTests);

	const TArray<FString> FilterTokens = ParseFilterTokens(TestFilter);
	for (const FString& TestName : AllVisibleTests)
	{
		if (MatchesAnyFilterToken(TestName, FilterTokens))
		{
			SelectedTests.AddUnique(TestName);
		}
	}

	if (SelectedTests.Num() == 0)
	{
		FinishWithExitCode(2, FString::Printf(TEXT("No automation tests matched filter '%s'"), *TestFilter));
		return;
	}

	for (const FString& TestName : SelectedTests)
	{
		UE_LOG(LogAgentBridgeGauntlet, Log, TEXT("[AgentBridge Gauntlet] EnableTest: %s"), *TestName);
	}

	AutomationController->StopTests();
	AutomationController->SetEnabledTests(SelectedTests);
	AutomationController->RunTests(true);

	WaitTimer = 0.0f;
	PollAccumulator = 0.0f;
	CurrentState = EAgentBridgeGauntletState::WaitingForAutomationRun;

	UE_LOG(
		LogAgentBridgeGauntlet,
		Log,
		TEXT("[AgentBridge Gauntlet] RunTests(LocalSession=true) triggered. Selected=%d"),
		SelectedTests.Num());
}

int32 UAgentBridgeGauntletController::BuildAutomationExitCode(FString& OutSummary) const
{
	if (!AutomationController.IsValid())
	{
		OutSummary = TEXT("AutomationController unavailable while building result");
		return 2;
	}

	int32 PassedCount = 0;
	int32 WarningCount = 0;
	int32 FailedCount = 0;
	int32 NotRunCount = 0;
	int32 InProcessCount = 0;
	int32 LeafReportCount = 0;
	TArray<FString> FailedTests;

	const TArray<TSharedPtr<IAutomationReport>> Reports = AutomationController->GetEnabledReports();
	for (const TSharedPtr<IAutomationReport>& Report : Reports)
	{
		if (!Report.IsValid() || Report->GetTotalNumChildren() > 0)
		{
			continue;
		}

		LeafReportCount++;

		const EAutomationState State = Report->GetState(0, 0);
		switch (State)
		{
		case EAutomationState::Success:
			if (Report->HasWarnings())
			{
				WarningCount++;
			}
			else
			{
				PassedCount++;
			}
			break;

		case EAutomationState::Fail:
			FailedCount++;
			FailedTests.Add(Report->GetFullTestPath());
			break;

		case EAutomationState::InProcess:
			InProcessCount++;
			break;

		case EAutomationState::NotRun:
		default:
			NotRunCount++;
			break;
		}
	}

	OutSummary = FString::Printf(
		TEXT("Selected=%d LeafReports=%d Passed=%d Warnings=%d Failed=%d NotRun=%d InProcess=%d"),
		SelectedTests.Num(),
		LeafReportCount,
		PassedCount,
		WarningCount,
		FailedCount,
		NotRunCount,
		InProcessCount);

	if (FailedTests.Num() > 0)
	{
		OutSummary += FString::Printf(TEXT(" | FailedTests=%s"), *FString::Join(FailedTests, TEXT(", ")));
	}

	if (LeafReportCount == 0)
	{
		return 2;
	}

	if (FailedCount > 0 || AutomationController->ReportsHaveErrors())
	{
		return 1;
	}

	if (NotRunCount > 0 || InProcessCount > 0)
	{
		return 2;
	}

	return 0;
}

bool UAgentBridgeGauntletController::LaunchSpecProcess(FString& OutError)
{
	FString AbsoluteSpecPath = SpecPath;
	if (!FPaths::FileExists(AbsoluteSpecPath))
	{
		AbsoluteSpecPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / SpecPath);
	}

	if (!FPaths::FileExists(AbsoluteSpecPath))
	{
		OutError = FString::Printf(TEXT("Spec file not found: %s"), *SpecPath);
		return false;
	}

	SpecReportPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("AgentBridge/Gauntlet/gauntlet_spec_execution_report.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SpecReportPath), true);

	const FString ProjectFilePath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString ExecutablePath = ResolveCommandletExecutablePath();
	const FString Params = FString::Printf(
		TEXT("\"%s\" -run=AgentBridge -Spec=\"%s\" -Report=\"%s\" -Unattended -NoSplash -NoSound -stdout -FullStdOutLogOutput"),
		*ProjectFilePath,
		*AbsoluteSpecPath,
		*SpecReportPath);

	UE_LOG(
		LogAgentBridgeGauntlet,
		Log,
		TEXT("[AgentBridge Gauntlet] Launch spec commandlet: %s %s"),
		*ExecutablePath,
		*Params);

	SpecProcessHandle = FPlatformProcess::CreateProc(
		*ExecutablePath,
		*Params,
		true,
		false,
		false,
		nullptr,
		0,
		nullptr,
		nullptr);

	if (!SpecProcessHandle.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to launch spec commandlet: %s"), *ExecutablePath);
		return false;
	}

	return true;
}

void UAgentBridgeGauntletController::HandleTestsRefreshed()
{
	bTestsRefreshed = true;
	UE_LOG(
		LogAgentBridgeGauntlet,
		Log,
		TEXT("[AgentBridge Gauntlet] Tests refreshed. DeviceClusters=%d"),
		AutomationController.IsValid() ? AutomationController->GetNumDeviceClusters() : -1);
}

void UAgentBridgeGauntletController::FinishWithExitCode(int32 ExitCode, const FString& Reason)
{
	if (CurrentState == EAgentBridgeGauntletState::Finished)
	{
		return;
	}

	if (AutomationController.IsValid())
	{
		if (TestsRefreshedHandle.IsValid())
		{
			AutomationController->OnTestsRefreshed().Remove(TestsRefreshedHandle);
			TestsRefreshedHandle.Reset();
		}
	}

	CurrentState = EAgentBridgeGauntletState::Finished;
	UE_LOG(LogAgentBridgeGauntlet, Log, TEXT("[AgentBridge Gauntlet] Finish ExitCode=%d, Reason=%s"), ExitCode, *Reason);
	EndTest(ExitCode);
}

