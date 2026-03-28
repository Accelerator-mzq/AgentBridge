// AgentBridgeCommandlet.cpp
// AGENT + UE5 可操作层 - Commandlet 无头执行实现

#include "AgentBridgeCommandlet.h"

#include "AgentBridgeSubsystem.h"
#include "BridgeTypes.h"
#include "Editor.h"
#include "IPythonScriptPlugin.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// 将 status 字符串映射到 EBridgeStatus。
	static EBridgeStatus ParseStatusString(const FString& StatusString)
	{
		const FString Lower = StatusString.ToLower();
		if (Lower == TEXT("success")) return EBridgeStatus::Success;
		if (Lower == TEXT("warning")) return EBridgeStatus::Warning;
		if (Lower == TEXT("mismatch")) return EBridgeStatus::Mismatch;
		if (Lower == TEXT("validation_error")) return EBridgeStatus::ValidationError;
		return EBridgeStatus::Failed;
	}

	// 构造简易 JSON 结果，保证 report 文件始终是合法 JSON。
	static FString MakeSimpleResultJson(const EBridgeStatus Status, const FString& Summary)
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());
		Root->SetStringField(TEXT("status"), BridgeStatusToString(Status));
		Root->SetStringField(TEXT("summary"), Summary);
		Root->SetObjectField(TEXT("data"), MakeShareable(new FJsonObject()));
		Root->SetArrayField(TEXT("warnings"), {});
		Root->SetArrayField(TEXT("errors"), {});

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Json;
	}

	/** 获取可用于无头子进程的 UnrealEditor-Cmd 路径。 */
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

	/** 尝试定位嵌套测试插件，便于 -RunTests 模式在无头子进程中显式加载。 */
	static FString ResolveAgentBridgeTestsPluginPath()
	{
		const FString Candidate = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Plugins/AgentBridge/AgentBridgeTests/AgentBridgeTests.uplugin"));
		return FPaths::FileExists(Candidate) ? Candidate : FString();
	}

	/** 根据退出码映射桥接状态。 */
	static EBridgeStatus ExitCodeToBridgeStatus(const int32 ExitCode)
	{
		if (ExitCode == 0)
		{
			return EBridgeStatus::Success;
		}
		if (ExitCode == 1)
		{
			return EBridgeStatus::Mismatch;
		}
		return EBridgeStatus::Failed;
	}
}

UAgentBridgeCommandlet::UAgentBridgeCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;

	HelpDescription = TEXT("AgentBridge headless entry for Spec/Test/Tool execution");
	HelpUsage = TEXT("UnrealEditor-Cmd.exe Project.uproject -run=AgentBridge [-Spec=xxx.yaml | -RunTests=Filter | -Tool=Name] [-Report=path]");

	HelpParamNames = {
		TEXT("Spec"),
		TEXT("RunTests"),
		TEXT("Tool"),
		TEXT("Report")
	};
	HelpParamDescriptions = {
		TEXT("YAML spec path"),
		TEXT("Automation test filter"),
		TEXT("Single tool name"),
		TEXT("JSON report output path")
	};
}

int32 UAgentBridgeCommandlet::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("[AgentBridge Commandlet] Params: %s"), *Params);

	ParseParams(Params);
	LastResultJson = TEXT("");

	int32 ExitCode = 2;
	if (!SpecPath.IsEmpty())
	{
		ExitCode = RunSpec();
	}
	else if (!TestFilter.IsEmpty())
	{
		ExitCode = RunTests();
	}
	else if (!ToolName.IsEmpty())
	{
		ExitCode = RunSingleTool();
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge Commandlet] No mode specified. Use -Spec / -RunTests / -Tool."));
		LastResultJson = MakeSimpleResultJson(EBridgeStatus::ValidationError, TEXT("No mode specified"));
		ExitCode = 2;
	}

	if (!ReportPath.IsEmpty())
	{
		if (LastResultJson.IsEmpty())
		{
			LastResultJson = MakeSimpleResultJson(
				ExitCode == 0 ? EBridgeStatus::Success : (ExitCode == 1 ? EBridgeStatus::Warning : EBridgeStatus::Failed),
				TEXT("Commandlet finished without explicit JSON payload"));
		}
		WriteReport(LastResultJson);
	}

	// 始终输出最终 JSON，便于命令行/CI 直接采集结果证据。
	if (!LastResultJson.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("[AgentBridge Commandlet] ResultJson=%s"), *LastResultJson);
	}

	UE_LOG(LogTemp, Log, TEXT("[AgentBridge Commandlet] ExitCode=%d"), ExitCode);
	return ExitCode;
}

void UAgentBridgeCommandlet::ParseParams(const FString& Params)
{
	RawParams = Params;
	SpecPath.Empty();
	TestFilter.Empty();
	ToolName.Empty();
	ReportPath.Empty();

	FParse::Value(*Params, TEXT("Spec="), SpecPath);
	FParse::Value(*Params, TEXT("RunTests="), TestFilter);
	FParse::Value(*Params, TEXT("Tool="), ToolName);
	FParse::Value(*Params, TEXT("Report="), ReportPath);

	SpecPath.TrimQuotesInline();
	TestFilter.TrimQuotesInline();
	ToolName.TrimQuotesInline();
	ReportPath.TrimQuotesInline();
}

FString UAgentBridgeCommandlet::GetNamedParam(const TCHAR* Key) const
{
	FString Value;
	FParse::Value(*RawParams, Key, Value);
	Value.TrimQuotesInline();
	return Value;
}

bool UAgentBridgeCommandlet::GetBoolNamedParam(const TCHAR* Key, const bool bDefaultValue) const
{
	const FString Value = GetNamedParam(Key).ToLower();
	if (Value.IsEmpty())
	{
		return bDefaultValue;
	}
	if (Value == TEXT("1") || Value == TEXT("true") || Value == TEXT("yes") || Value == TEXT("on"))
	{
		return true;
	}
	if (Value == TEXT("0") || Value == TEXT("false") || Value == TEXT("no") || Value == TEXT("off"))
	{
		return false;
	}
	return bDefaultValue;
}

bool UAgentBridgeCommandlet::TryGetVectorParam(const TCHAR* Key, FVector& OutVector) const
{
	const FString Value = GetNamedParam(Key);
	if (Value.IsEmpty())
	{
		return false;
	}

	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	if (!ParseCsv3(Value, X, Y, Z))
	{
		return false;
	}

	OutVector = FVector(X, Y, Z);
	return true;
}

bool UAgentBridgeCommandlet::TryGetRotatorParam(const TCHAR* Key, FRotator& OutRotator) const
{
	const FString Value = GetNamedParam(Key);
	if (Value.IsEmpty())
	{
		return false;
	}

	double Pitch = 0.0;
	double Yaw = 0.0;
	double Roll = 0.0;
	if (!ParseCsv3(Value, Pitch, Yaw, Roll))
	{
		return false;
	}

	OutRotator = FRotator(Pitch, Yaw, Roll);
	return true;
}

bool UAgentBridgeCommandlet::ParseCsv3(const FString& Csv, double& X, double& Y, double& Z)
{
	TArray<FString> Parts;
	Csv.ParseIntoArray(Parts, TEXT(","), true);
	if (Parts.Num() != 3)
	{
		return false;
	}

	return LexTryParseString(X, *Parts[0].TrimStartAndEnd())
		&& LexTryParseString(Y, *Parts[1].TrimStartAndEnd())
		&& LexTryParseString(Z, *Parts[2].TrimStartAndEnd());
}

int32 UAgentBridgeCommandlet::StatusToExitCode(const EBridgeStatus Status)
{
	switch (Status)
	{
	case EBridgeStatus::Success:
		return 0;
	case EBridgeStatus::Warning:
	case EBridgeStatus::Mismatch:
		return 1;
	case EBridgeStatus::ValidationError:
	case EBridgeStatus::Failed:
	default:
		return 2;
	}
}

int32 UAgentBridgeCommandlet::RunSpec()
{
	const TSharedPtr<IPlugin> AgentBridgePlugin = IPluginManager::Get().FindPlugin(TEXT("AgentBridge"));
	const FString PluginRootDir = AgentBridgePlugin.IsValid()
		? FPaths::ConvertRelativePathToFull(AgentBridgePlugin->GetBaseDir())
		: FString();

	FString AbsoluteSpecPath = SpecPath;
	if (!FPaths::FileExists(AbsoluteSpecPath))
	{
		AbsoluteSpecPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / SpecPath);
	}
	if (!FPaths::FileExists(AbsoluteSpecPath) && !PluginRootDir.IsEmpty())
	{
		// 重构后 Spec 已迁到 Plugins/AgentBridge/Specs，命令行仍可能传旧的 Specs/... 相对路径。
		AbsoluteSpecPath = FPaths::ConvertRelativePathToFull(PluginRootDir / SpecPath);
	}

	if (!FPaths::FileExists(AbsoluteSpecPath))
	{
		LastResultJson = MakeSimpleResultJson(EBridgeStatus::ValidationError, FString::Printf(TEXT("Spec not found: %s"), *SpecPath));
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge Commandlet] Spec file not found: %s"), *AbsoluteSpecPath);
		return 2;
	}

	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
	{
		LastResultJson = MakeSimpleResultJson(EBridgeStatus::Failed, TEXT("PythonScriptPlugin is not available"));
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge Commandlet] PythonScriptPlugin unavailable"));
		return 2;
	}

	FString EffectiveReportPath = ReportPath;
	if (EffectiveReportPath.IsEmpty())
	{
		EffectiveReportPath = FPaths::ProjectSavedDir() / TEXT("AgentBridge/commandlet_spec_report.json");
	}
	if (!FPaths::IsRelative(EffectiveReportPath))
	{
		EffectiveReportPath = FPaths::ConvertRelativePathToFull(EffectiveReportPath);
	}
	else
	{
		EffectiveReportPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / EffectiveReportPath);
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(EffectiveReportPath), true);

	auto ToPythonLiteral = [](FString Path) -> FString
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		Path.ReplaceInline(TEXT("'"), TEXT("\\'"));
		return Path;
	};

	const FString ProjectDirPy = ToPythonLiteral(FPaths::ProjectDir());
	const FString ScriptsDirPy = ToPythonLiteral(
		!PluginRootDir.IsEmpty()
			? (PluginRootDir / TEXT("Scripts"))
			: (FPaths::ProjectDir() / TEXT("Scripts")));
	const FString SpecPy = ToPythonLiteral(AbsoluteSpecPath);
	const FString ReportPy = ToPythonLiteral(EffectiveReportPath);

	const FString PythonCommand = FString::Printf(
		TEXT("import json,sys; ")
		TEXT("sys.path.insert(0,r'%s'); ")
		TEXT("sys.path.insert(0,r'%s'); ")
		TEXT("from orchestrator import run as _ab_run; ")
		TEXT("_ab_result=_ab_run(r'%s'); ")
		TEXT("open(r'%s','w',encoding='utf-8').write(json.dumps(_ab_result, ensure_ascii=False, indent=2, default=str))"),
		*ProjectDirPy,
		*ScriptsDirPy,
		*SpecPy,
		*ReportPy);

	const bool bExecOk = PythonPlugin->ExecPythonCommand(*PythonCommand);
	if (!bExecOk)
	{
		LastResultJson = MakeSimpleResultJson(EBridgeStatus::Failed, TEXT("Python orchestrator execution failed"));
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge Commandlet] Python execution failed"));
		return 2;
	}

	if (!FFileHelper::LoadFileToString(LastResultJson, *EffectiveReportPath))
	{
		LastResultJson = MakeSimpleResultJson(EBridgeStatus::Success, TEXT("Spec executed, but report file is not readable"));
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge Commandlet] Spec report not readable: %s"), *EffectiveReportPath);
		return 0;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(LastResultJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge Commandlet] Spec report is not valid JSON, fallback to success"));
		return 0;
	}

	if (Root->HasTypedField<EJson::String>(TEXT("status")))
	{
		return StatusToExitCode(ParseStatusString(Root->GetStringField(TEXT("status"))));
	}

	// Orchestrator 报告走的是 overall_status 口径，这里补齐映射，保证 -Spec 模式退出码可判定。
	if (Root->HasTypedField<EJson::String>(TEXT("overall_status")))
	{
		const FString OverallStatus = Root->GetStringField(TEXT("overall_status")).ToLower();
		if (OverallStatus == TEXT("success"))
		{
			return 0;
		}
		if (OverallStatus == TEXT("warning") || OverallStatus == TEXT("mismatch"))
		{
			return 1;
		}
		return 2;
	}

	if (const TArray<TSharedPtr<FJsonValue>>* Items = nullptr; Root->TryGetArrayField(TEXT("results"), Items) && Items)
	{
		EBridgeStatus WorstStatus = EBridgeStatus::Success;
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			if (!Item.IsValid() || Item->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Obj = Item->AsObject();
			if (!Obj.IsValid() || !Obj->HasTypedField<EJson::String>(TEXT("status")))
			{
				continue;
			}
			const EBridgeStatus ItemStatus = ParseStatusString(Obj->GetStringField(TEXT("status")));
			if (StatusToExitCode(ItemStatus) > StatusToExitCode(WorstStatus))
			{
				WorstStatus = ItemStatus;
			}
		}
		return StatusToExitCode(WorstStatus);
	}

	return 0;
}

int32 UAgentBridgeCommandlet::RunTests()
{
	const FString ExecutablePath = ResolveCommandletExecutablePath();
	if (!FPaths::FileExists(ExecutablePath))
	{
		LastResultJson = MakeSimpleResultJson(EBridgeStatus::Failed, TEXT("Unable to resolve UnrealEditor-Cmd executable"));
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge Commandlet] UnrealEditor-Cmd executable not found: %s"), *ExecutablePath);
		return 2;
	}

	const FString AbsoluteProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString AbsoluteReportPath = ReportPath.IsEmpty()
		? FString()
		: FPaths::ConvertRelativePathToFull(FPaths::IsRelative(ReportPath) ? (FPaths::ProjectDir() / ReportPath) : ReportPath);
	const FString ChildLogPath = AbsoluteReportPath.IsEmpty()
		? FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AgentBridge/commandlet_run_tests.log"))
		: FPaths::Combine(FPaths::GetPath(AbsoluteReportPath), FString::Printf(TEXT("%s_automation.log"), *FPaths::GetBaseFilename(AbsoluteReportPath)));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ChildLogPath), true);

	// 测试插件默认不随普通 Editor receipt 常驻启用，这里显式补齐描述文件路径和插件开关。
	const FString TestsPluginPath = ResolveAgentBridgeTestsPluginPath();
	const FString TestsPluginArgs = TestsPluginPath.IsEmpty()
		? FString()
		: FString::Printf(TEXT(" -PLUGIN=\"%s\" -EnablePlugins=AgentBridgeTests"), *TestsPluginPath);

	FString Args = FString::Printf(
		TEXT("\"%s\"%s -ExecCmds=\"Automation RunTests %s;Quit\" -TestExit=\"Automation Test Queue Empty\" -Unattended -NoSplash -NoPause -NoSound -NullRHI -stdout -FullStdOutLogOutput -Abslog=\"%s\""),
		*AbsoluteProjectPath,
		*TestsPluginArgs,
		*TestFilter,
		*ChildLogPath);

	uint32 ProcessId = 0;
	void* StdOutReadPipe = nullptr;
	void* StdOutWritePipe = nullptr;
	FPlatformProcess::CreatePipe(StdOutReadPipe, StdOutWritePipe);

	UE_LOG(LogTemp, Log, TEXT("[AgentBridge Commandlet] Launch RunTests child: %s %s"), *ExecutablePath, *Args);
	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		*ExecutablePath,
		*Args,
		true,
		true,
		false,
		&ProcessId,
		0,
		nullptr,
		StdOutWritePipe,
		nullptr);

	if (!ProcHandle.IsValid())
	{
		if (StdOutReadPipe || StdOutWritePipe)
		{
			FPlatformProcess::ClosePipe(StdOutReadPipe, StdOutWritePipe);
		}

		LastResultJson = MakeSimpleResultJson(EBridgeStatus::Failed, TEXT("Failed to launch headless automation child process"));
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge Commandlet] Failed to launch child process for RunTests"));
		return 2;
	}

	FString ChildStdOut;
	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		if (StdOutReadPipe)
		{
			ChildStdOut += FPlatformProcess::ReadPipe(StdOutReadPipe);
		}
		FPlatformProcess::Sleep(0.05f);
	}

	FPlatformProcess::WaitForProc(ProcHandle);
	if (StdOutReadPipe)
	{
		ChildStdOut += FPlatformProcess::ReadPipe(StdOutReadPipe);
	}

	int32 ChildExitCode = -1;
	if (!FPlatformProcess::GetProcReturnCode(ProcHandle, &ChildExitCode))
	{
		ChildExitCode = -1;
	}

	if (StdOutReadPipe || StdOutWritePipe)
	{
		FPlatformProcess::ClosePipe(StdOutReadPipe, StdOutWritePipe);
	}
	FPlatformProcess::CloseProc(ProcHandle);

	FString ChildLogContent;
	FFileHelper::LoadFileToString(ChildLogContent, *ChildLogPath);

	const bool bNoTestsMatched = ChildStdOut.Contains(TEXT("No automation tests matched"), ESearchCase::IgnoreCase)
		|| ChildLogContent.Contains(TEXT("No automation tests matched"), ESearchCase::IgnoreCase);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("filter"), TestFilter);
	Data->SetStringField(TEXT("child_command_line"), FString::Printf(TEXT("%s %s"), *ExecutablePath, *Args));
	Data->SetStringField(TEXT("child_log_path"), ChildLogPath);
	Data->SetNumberField(TEXT("child_exit_code"), ChildExitCode);
	Data->SetBoolField(TEXT("tests_plugin_explicitly_loaded"), !TestsPluginPath.IsEmpty());
	if (!TestsPluginPath.IsEmpty())
	{
		Data->SetStringField(TEXT("tests_plugin_path"), TestsPluginPath);
	}
	Data->SetBoolField(TEXT("no_tests_matched"), bNoTestsMatched);
	Data->SetNumberField(TEXT("stdout_length"), ChildStdOut.Len());

	FBridgeResponse Response;
	Response.Data = Data;

	if (bNoTestsMatched)
	{
		Response.Status = EBridgeStatus::Failed;
		Response.Summary = FString::Printf(TEXT("No automation tests matched filter '%s'"), *TestFilter);
	}
	else
	{
		Response.Status = ExitCodeToBridgeStatus(ChildExitCode);
		Response.Summary = FString::Printf(TEXT("Automation child finished with exit code %d"), ChildExitCode);
	}

	Response.SyncForRemote();
	LastResultJson = Response.ToJsonString();
	UE_LOG(LogTemp, Log, TEXT("[AgentBridge Commandlet] RunTests status=%s summary=%s"), *BridgeStatusToString(Response.Status), *Response.Summary);
	return StatusToExitCode(Response.Status);
}

int32 UAgentBridgeCommandlet::RunSingleTool()
{
	if (!GEditor)
	{
		LastResultJson = MakeSimpleResultJson(EBridgeStatus::Failed, TEXT("GEditor is null"));
		return 2;
	}

	UAgentBridgeSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>();
	if (!Subsystem)
	{
		LastResultJson = MakeSimpleResultJson(EBridgeStatus::Failed, TEXT("AgentBridgeSubsystem unavailable"));
		return 2;
	}

	FBridgeResponse Response;

	// 查询工具（7）
	if (ToolName.Equals(TEXT("GetCurrentProjectState"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->GetCurrentProjectState();
	}
	else if (ToolName.Equals(TEXT("ListLevelActors"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->ListLevelActors(GetNamedParam(TEXT("ClassFilter=")));
	}
	else if (ToolName.Equals(TEXT("GetActorState"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->GetActorState(GetNamedParam(TEXT("ActorPath=")));
	}
	else if (ToolName.Equals(TEXT("GetActorBounds"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->GetActorBounds(GetNamedParam(TEXT("ActorPath=")));
	}
	else if (ToolName.Equals(TEXT("GetAssetMetadata"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->GetAssetMetadata(GetNamedParam(TEXT("AssetPath=")));
	}
	else if (ToolName.Equals(TEXT("GetDirtyAssets"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->GetDirtyAssets();
	}
	else if (ToolName.Equals(TEXT("RunMapCheck"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->RunMapCheck();
	}
	// 写工具（4）
	else if (ToolName.Equals(TEXT("SpawnActor"), ESearchCase::IgnoreCase))
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		if (!TryGetVectorParam(TEXT("Location="), Location)
			|| !TryGetRotatorParam(TEXT("Rotation="), Rotation)
			|| !TryGetVectorParam(TEXT("Scale="), Scale))
		{
			Response = AgentBridge::MakeValidationError(TEXT("Transform"), TEXT("SpawnActor requires -Location=x,y,z -Rotation=p,y,r -Scale=x,y,z"));
		}
		else
		{
			FBridgeTransform Transform;
			Transform.Location = Location;
			Transform.Rotation = Rotation;
			Transform.RelativeScale3D = Scale;
			Response = Subsystem->SpawnActor(
				GetNamedParam(TEXT("LevelPath=")),
				GetNamedParam(TEXT("ActorClass=")),
				GetNamedParam(TEXT("ActorName=")),
				Transform,
				GetBoolNamedParam(TEXT("bDryRun="), false));
		}
	}
	else if (ToolName.Equals(TEXT("SetActorTransform"), ESearchCase::IgnoreCase))
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;
		if (!TryGetVectorParam(TEXT("Location="), Location)
			|| !TryGetRotatorParam(TEXT("Rotation="), Rotation)
			|| !TryGetVectorParam(TEXT("Scale="), Scale))
		{
			Response = AgentBridge::MakeValidationError(TEXT("Transform"), TEXT("SetActorTransform requires -Location=x,y,z -Rotation=p,y,r -Scale=x,y,z"));
		}
		else
		{
			FBridgeTransform Transform;
			Transform.Location = Location;
			Transform.Rotation = Rotation;
			Transform.RelativeScale3D = Scale;
			Response = Subsystem->SetActorTransform(
				GetNamedParam(TEXT("ActorPath=")),
				Transform,
				GetBoolNamedParam(TEXT("bDryRun="), false));
		}
	}
	else if (ToolName.Equals(TEXT("ImportAssets"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->ImportAssets(
			GetNamedParam(TEXT("SourceDir=")),
			GetNamedParam(TEXT("DestPath=")),
			GetBoolNamedParam(TEXT("bReplaceExisting="), false),
			GetBoolNamedParam(TEXT("bDryRun="), false));
	}
	else if (ToolName.Equals(TEXT("CreateBlueprintChild"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->CreateBlueprintChild(
			GetNamedParam(TEXT("ParentClass=")),
			GetNamedParam(TEXT("PackagePath=")),
			GetBoolNamedParam(TEXT("bDryRun="), false));
	}
	// 验证工具（3）
	else if (ToolName.Equals(TEXT("ValidateActorInsideBounds"), ESearchCase::IgnoreCase))
	{
		FVector BoundsOrigin = FVector::ZeroVector;
		FVector BoundsExtent = FVector::ZeroVector;
		if (!TryGetVectorParam(TEXT("BoundsOrigin="), BoundsOrigin)
			|| !TryGetVectorParam(TEXT("BoundsExtent="), BoundsExtent))
		{
			Response = AgentBridge::MakeValidationError(TEXT("Bounds"), TEXT("ValidateActorInsideBounds requires -BoundsOrigin=x,y,z -BoundsExtent=x,y,z"));
		}
		else
		{
			Response = Subsystem->ValidateActorInsideBounds(
				GetNamedParam(TEXT("ActorPath=")),
				BoundsOrigin,
				BoundsExtent);
		}
	}
	else if (ToolName.Equals(TEXT("ValidateActorNonOverlap"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->ValidateActorNonOverlap(GetNamedParam(TEXT("ActorPath=")));
	}
	else if (ToolName.Equals(TEXT("RunAutomationTests"), ESearchCase::IgnoreCase))
	{
		Response = Subsystem->RunAutomationTests(GetNamedParam(TEXT("Filter=")), GetNamedParam(TEXT("TestReportPath=")));
	}
	// 构建工具（1）
	else if (ToolName.Equals(TEXT("BuildProject"), ESearchCase::IgnoreCase))
	{
		const FString Platform = GetNamedParam(TEXT("Platform=")).IsEmpty() ? TEXT("Win64") : GetNamedParam(TEXT("Platform="));
		const FString Configuration = GetNamedParam(TEXT("Configuration=")).IsEmpty() ? TEXT("Development") : GetNamedParam(TEXT("Configuration="));
		Response = Subsystem->BuildProject(Platform, Configuration, GetBoolNamedParam(TEXT("bDryRun="), false));
	}
	else
	{
		Response = AgentBridge::MakeValidationError(
			TEXT("Tool"),
			FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge Commandlet] Unknown tool: %s"), *ToolName);
	}

	LastResultJson = Response.ToJsonString();
	UE_LOG(LogTemp, Log, TEXT("[AgentBridge Commandlet] Tool=%s status=%s summary=%s"), *ToolName, *BridgeStatusToString(Response.Status), *Response.Summary);
	return StatusToExitCode(Response.Status);
}

void UAgentBridgeCommandlet::WriteReport(const FString& JsonContent)
{
	if (ReportPath.IsEmpty())
	{
		return;
	}

	FString FinalPath = ReportPath;
	if (FPaths::IsRelative(FinalPath))
	{
		FinalPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / FinalPath);
	}
	else
	{
		FinalPath = FPaths::ConvertRelativePathToFull(FinalPath);
	}

	const FString Dir = FPaths::GetPath(FinalPath);
	if (!Dir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Dir, true);
	}

	if (FFileHelper::SaveStringToFile(JsonContent, *FinalPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Log, TEXT("[AgentBridge Commandlet] Report written: %s"), *FinalPath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[AgentBridge Commandlet] Failed to write report: %s"), *FinalPath);
	}
}

