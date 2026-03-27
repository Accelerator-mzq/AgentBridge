// L3_FunctionalTestActor.cpp
// AGENT + UE5 可操作层 — L3 完整 Demo 验证实现
//
// 目标：
//   1. 内置模式：在 Functional Test 地图中执行多 Actor Spawn -> Readback -> 容差验证
//   2. Spec 驱动：通过 Python Orchestrator 执行结构化 Spec，并解析最终报告
//   3. 测试结束后按事务快照做 Undo 清理，避免关卡残留

#include "L3_FunctionalTestActor.h"

#include "AgentBridgeSubsystem.h"
#include "BridgeTypes.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IPythonScriptPlugin.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	static bool TryReadJsonNumberArray(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		FVector& OutVector)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (!Object->TryGetArrayField(FieldName, ArrayPtr) || !ArrayPtr || ArrayPtr->Num() != 3)
		{
			return false;
		}

		OutVector = FVector(
			static_cast<float>((*ArrayPtr)[0]->AsNumber()),
			static_cast<float>((*ArrayPtr)[1]->AsNumber()),
			static_cast<float>((*ArrayPtr)[2]->AsNumber()));
		return true;
	}

	static bool TryReadJsonNumberArray(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		FRotator& OutRotator)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (!Object->TryGetArrayField(FieldName, ArrayPtr) || !ArrayPtr || ArrayPtr->Num() != 3)
		{
			return false;
		}

		OutRotator = FRotator(
			static_cast<float>((*ArrayPtr)[0]->AsNumber()),
			static_cast<float>((*ArrayPtr)[1]->AsNumber()),
			static_cast<float>((*ArrayPtr)[2]->AsNumber()));
		return true;
	}

	static bool TryExtractTransform(
		const TSharedPtr<FJsonObject>& DataObject,
		FBridgeTransform& OutTransform)
	{
		if (!DataObject.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* TransformObject = nullptr;
		if (!DataObject->TryGetObjectField(TEXT("transform"), TransformObject) || !TransformObject || !TransformObject->IsValid())
		{
			return false;
		}

		FBridgeTransform Parsed;
		if (!TryReadJsonNumberArray(*TransformObject, TEXT("location"), Parsed.Location))
		{
			return false;
		}
		if (!TryReadJsonNumberArray(*TransformObject, TEXT("rotation"), Parsed.Rotation))
		{
			return false;
		}
		if (!TryReadJsonNumberArray(*TransformObject, TEXT("relative_scale3d"), Parsed.RelativeScale3D))
		{
			return false;
		}

		OutTransform = Parsed;
		return true;
	}

	static FString ToPythonLiteral(const FString& InPath)
	{
		FString Normalized = FPaths::ConvertRelativePathToFull(InPath);
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		Normalized.ReplaceInline(TEXT("'"), TEXT("\\'"));
		return Normalized;
	}

	static TSubclassOf<AActor> ResolveBuiltInActorClass(const FString& ActorClassPath)
	{
		if (ActorClassPath.Equals(TEXT("/Script/Engine.StaticMeshActor"), ESearchCase::IgnoreCase)
			|| ActorClassPath.Equals(TEXT("StaticMeshActor"), ESearchCase::IgnoreCase))
		{
			return AStaticMeshActor::StaticClass();
		}

		if (ActorClassPath.Equals(TEXT("/Script/Engine.PointLight"), ESearchCase::IgnoreCase)
			|| ActorClassPath.Equals(TEXT("PointLight"), ESearchCase::IgnoreCase))
		{
			return APointLight::StaticClass();
		}

		if (UClass* LoadedClass = StaticLoadClass(AActor::StaticClass(), nullptr, *ActorClassPath))
		{
			return LoadedClass;
		}

		return nullptr;
	}

	static FBox ComputeActorBoundsBox(const AActor* Actor)
	{
		FVector Origin = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		Actor->GetActorBounds(false, Origin, Extent);
		return FBox::BuildAABB(Origin, Extent);
	}

	static bool HasMeaningfulBounds(const AActor* Actor)
	{
		FVector Origin = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		Actor->GetActorBounds(false, Origin, Extent);
		return !Extent.IsNearlyZero(KINDA_SMALL_NUMBER);
	}
}

AAgentBridgeFunctionalTest::AAgentBridgeFunctionalTest()
{
	TestLabel = TEXT("AgentBridge L3 Full Demo");
}

bool AAgentBridgeFunctionalTest::IsReady_Implementation()
{
	if (!GEditor)
	{
		return false;
	}

	if (!GEditor->GetEditorWorldContext().World())
	{
		return false;
	}

	return GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>() != nullptr;
}

void AAgentBridgeFunctionalTest::PrepareTest()
{
	Super::PrepareTest();

	CachedSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>() : nullptr;
	SpawnedActorPaths.Empty();
	SpawnedActorRefs.Empty();
	LastReportPath.Empty();
	LastExecutionSummary.Empty();
	TransactionDepthBeforeTest = GetCommittedTransactionDepth();

	LogMessage(FString::Printf(
		TEXT("[AgentBridge L3] PrepareTest: SpecPath=%s, BuiltInActorCount=%d, TransactionDepthBefore=%d"),
		SpecPath.IsEmpty() ? TEXT("(built-in)") : *SpecPath,
		BuiltInActorCount,
		TransactionDepthBeforeTest));
}

void AAgentBridgeFunctionalTest::StartTest()
{
	Super::StartTest();

	if (!CachedSubsystem)
	{
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(TEXT("Subsystem not available")));
		return;
	}

	if (SpecPath.IsEmpty())
	{
		RunBuiltInScenario();
		return;
	}

	RunSpecDriven();
}

void AAgentBridgeFunctionalTest::CleanUp()
{
	if (bUndoAfterTest && GEditor && GEditor->Trans)
	{
		const int32 DepthAfterTest = GetCommittedTransactionDepth();
		const int32 UndoSteps = FMath::Max(DepthAfterTest - TransactionDepthBeforeTest, 0);

		LogMessage(FString::Printf(
			TEXT("[AgentBridge L3] CleanUp: TransactionDepthBefore=%d, After=%d, UndoSteps=%d"),
			TransactionDepthBeforeTest,
			DepthAfterTest,
			UndoSteps));

		for (int32 StepIndex = 0; StepIndex < UndoSteps; ++StepIndex)
		{
			if (!GEditor->UndoTransaction())
			{
				LogMessage(FString::Printf(
					TEXT("[AgentBridge L3] CleanUp: Undo stopped early at step %d"),
					StepIndex + 1));
				break;
			}
		}
	}

	// PIE 模式下内置场景直接在运行时世界中生成 Actor；这里优先显式销毁，避免测试结束前残留。
	for (const TWeakObjectPtr<AActor>& SpawnedActorRef : SpawnedActorRefs)
	{
		if (AActor* SpawnedActor = SpawnedActorRef.Get())
		{
			SpawnedActor->Destroy();
		}
	}

	SpawnedActorPaths.Empty();
	SpawnedActorRefs.Empty();
	LastReportPath.Empty();
	LastExecutionSummary.Empty();
	CachedSubsystem = nullptr;

	Super::CleanUp();
}

FString AAgentBridgeFunctionalTest::GetAdditionalTestFinishedMessage(EFunctionalTestResult TestResult) const
{
	return BuildFinishedMessage(LastExecutionSummary);
}

void AAgentBridgeFunctionalTest::RunBuiltInScenario()
{
	if (BuiltInActorCount <= 0)
	{
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(TEXT("BuiltInActorCount must be > 0")));
		return;
	}

	if (!GetWorld())
	{
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(TEXT("Functional Test world is not available")));
		return;
	}

	LogMessage(FString::Printf(
		TEXT("[AgentBridge L3] RunBuiltInScenario: backend=PIE direct world, PlayWorld=%s"),
		(GEditor && GEditor->PlayWorld) ? TEXT("true") : TEXT("false")));

	int32 PassedActors = 0;
	int32 FailedActors = 0;

	for (int32 Index = 0; Index < BuiltInActorCount; ++Index)
	{
		FBridgeTransform ExpectedTransform;
		ExpectedTransform.Location = FVector(300.0f * Index, 200.0f * Index, 50.0f * Index);
		ExpectedTransform.Rotation = FRotator(5.0f * Index, 30.0f * Index, 2.5f * Index);
		ExpectedTransform.RelativeScale3D = FVector(
			1.0f + 0.1f * Index,
			1.0f + 0.1f * Index,
			1.0f + 0.1f * Index);

		const FString ActorClass = (Index % 2 == 0)
			? TEXT("/Script/Engine.StaticMeshActor")
			: TEXT("/Script/Engine.PointLight");
		const FString ActorName = FString::Printf(TEXT("L3_BuiltIn_Actor_%02d"), Index);

		const bool bPassed = SpawnAndVerifyActor(
			Index,
			ActorClass,
			ActorName,
			ExpectedTransform,
			SpawnedActorPaths);
		if (bPassed)
		{
			++PassedActors;
		}
		else
		{
			++FailedActors;
		}
	}

	RunGlobalValidation(SpawnedActorPaths);
	LogTestReport(BuiltInActorCount, PassedActors, FailedActors);

	if (FailedActors > 0)
	{
		LastExecutionSummary = FString::Printf(TEXT("%d/%d built-in actors failed"), FailedActors, BuiltInActorCount);
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(LastExecutionSummary));
		return;
	}

	LastExecutionSummary = FString::Printf(TEXT("%d/%d built-in actors verified"), PassedActors, BuiltInActorCount);
	FinishTest(EFunctionalTestResult::Succeeded, BuildFinishedMessage(LastExecutionSummary));
}

void AAgentBridgeFunctionalTest::RunSpecDriven()
{
	if (!CachedSubsystem)
	{
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(TEXT("Subsystem not available")));
		return;
	}

	FString AbsoluteSpecPath = SpecPath;
	if (!FPaths::FileExists(AbsoluteSpecPath))
	{
		AbsoluteSpecPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / SpecPath);
	}

	if (!FPaths::FileExists(AbsoluteSpecPath))
	{
		FinishTest(
			EFunctionalTestResult::Failed,
			BuildFinishedMessage(FString::Printf(TEXT("Spec file not found: %s"), *SpecPath)));
		return;
	}

	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
	{
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(TEXT("PythonScriptPlugin is not available")));
		return;
	}

	LastReportPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("AgentBridge/functional_test_spec_report.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LastReportPath), true);

	const FString ProjectDirPy = ToPythonLiteral(FPaths::ProjectDir());
	const FString ScriptsDirPy = ToPythonLiteral(FPaths::ProjectDir() / TEXT("Scripts"));
	const FString SpecPy = ToPythonLiteral(AbsoluteSpecPath);
	const FString ReportPy = ToPythonLiteral(LastReportPath);

	const FString PythonCommand = FString::Printf(
		TEXT("import json,sys; ")
		TEXT("sys.path.insert(0,r'%s'); ")
		TEXT("sys.path.insert(0,r'%s'); ")
		TEXT("from orchestrator.orchestrator import run as _ab_run; ")
		TEXT("_ab_result=_ab_run(r'%s', report_path=r'%s'); ")
		TEXT("open(r'%s','w',encoding='utf-8').write(json.dumps(_ab_result, ensure_ascii=False, indent=2, default=str))"),
		*ProjectDirPy,
		*ScriptsDirPy,
		*SpecPy,
		*ReportPy,
		*ReportPy);

	LogMessage(FString::Printf(TEXT("[AgentBridge L3] RunSpecDriven: %s"), *AbsoluteSpecPath));

	if (!PythonPlugin->ExecPythonCommand(*PythonCommand))
	{
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(TEXT("Python orchestrator execution failed")));
		return;
	}

	FString ReportJson;
	if (!FFileHelper::LoadFileToString(ReportJson, *LastReportPath))
	{
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(TEXT("Spec report file is not readable")));
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReportJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(TEXT("Spec report is not valid JSON")));
		return;
	}

	const FString OverallStatus = RootObject->GetStringField(TEXT("overall_status"));
	const TSharedPtr<FJsonObject>* SummaryObject = nullptr;
	int32 TotalActors = 0;
	int32 PassedActors = 0;
	int32 FailedActors = 0;
	int32 MismatchedActors = 0;
	if (RootObject->TryGetObjectField(TEXT("summary"), SummaryObject) && SummaryObject && SummaryObject->IsValid())
	{
		TotalActors = static_cast<int32>((*SummaryObject)->GetNumberField(TEXT("total")));
		PassedActors = static_cast<int32>((*SummaryObject)->GetNumberField(TEXT("passed")));
		FailedActors = static_cast<int32>((*SummaryObject)->GetNumberField(TEXT("failed")));
		MismatchedActors = static_cast<int32>((*SummaryObject)->GetNumberField(TEXT("mismatched")));
	}

	LastExecutionSummary = FString::Printf(
		TEXT("Spec overall_status=%s, total=%d, passed=%d, mismatch=%d, failed=%d"),
		*OverallStatus,
		TotalActors,
		PassedActors,
		MismatchedActors,
		FailedActors);

	if (OverallStatus.Equals(TEXT("success"), ESearchCase::IgnoreCase))
	{
		FinishTest(EFunctionalTestResult::Succeeded, BuildFinishedMessage(LastExecutionSummary));
		return;
	}

	FinishTest(EFunctionalTestResult::Failed, BuildFinishedMessage(LastExecutionSummary));
}

bool AAgentBridgeFunctionalTest::SpawnAndVerifyActor(
	int32 Index,
	const FString& ActorClass,
	const FString& ActorName,
	const FBridgeTransform& ExpectedTransform,
	TArray<FString>& OutSpawnedPaths)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		LogMessage(TEXT("[AgentBridge L3] FAIL: Test world unavailable during SpawnAndVerifyActor"));
		return false;
	}

	const TSubclassOf<AActor> SpawnClass = ResolveBuiltInActorClass(ActorClass);
	if (!SpawnClass)
	{
		LogMessage(FString::Printf(
			TEXT("[AgentBridge L3] Actor %d FAIL: unsupported built-in class '%s'"),
			Index,
			*ActorClass));
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(World, SpawnClass, FName(*ActorName));
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags |= RF_Transient;

	const FTransform SpawnTransform(
		ExpectedTransform.Rotation,
		ExpectedTransform.Location,
		ExpectedTransform.RelativeScale3D);

	AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnClass, SpawnTransform, SpawnParameters);
	if (!SpawnedActor)
	{
		LogMessage(FString::Printf(
			TEXT("[AgentBridge L3] Actor %d FAIL: direct SpawnActor failed"),
			Index));
		return false;
	}

	#if WITH_EDITOR
	SpawnedActor->SetActorLabel(ActorName, false);
	#endif

	const FString ActorPath = SpawnedActor->GetPathName();
	OutSpawnedPaths.Add(ActorPath);
	SpawnedActorRefs.Add(SpawnedActor);

	const FBridgeTransform ActualTransform = FBridgeTransform::FromActor(SpawnedActor);

	const FVector LocationDelta = ActualTransform.Location - ExpectedTransform.Location;
	const FRotator RotationDelta = ActualTransform.Rotation - ExpectedTransform.Rotation;
	const FVector ScaleDelta = ActualTransform.RelativeScale3D - ExpectedTransform.RelativeScale3D;

	const bool bNearlyEqual = ExpectedTransform.NearlyEquals(
		ActualTransform,
		LocationTolerance,
		RotationTolerance,
		ScaleTolerance);

	if (!bNearlyEqual)
	{
		LogMessage(FString::Printf(
			TEXT("[AgentBridge L3] Actor %d FAIL: delta loc=(%.4f, %.4f, %.4f) rot=(%.4f, %.4f, %.4f) scale=(%.4f, %.4f, %.4f)"),
			Index,
			LocationDelta.X, LocationDelta.Y, LocationDelta.Z,
			RotationDelta.Pitch, RotationDelta.Yaw, RotationDelta.Roll,
			ScaleDelta.X, ScaleDelta.Y, ScaleDelta.Z));
		return false;
	}

	FVector BoundsOrigin = FVector::ZeroVector;
	FVector BoundsExtent = FVector::ZeroVector;
	SpawnedActor->GetActorBounds(false, BoundsOrigin, BoundsExtent);

	LogMessage(FString::Printf(
		TEXT("[AgentBridge L3] Actor %d PASS: %s | delta loc=(%.4f, %.4f, %.4f) rot=(%.4f, %.4f, %.4f) scale=(%.4f, %.4f, %.4f) bounds_extent=(%.4f, %.4f, %.4f)"),
		Index,
		*ActorName,
		LocationDelta.X, LocationDelta.Y, LocationDelta.Z,
		RotationDelta.Pitch, RotationDelta.Yaw, RotationDelta.Roll,
		ScaleDelta.X, ScaleDelta.Y, ScaleDelta.Z,
		BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z));
	return true;
}

void AAgentBridgeFunctionalTest::RunGlobalValidation(const TArray<FString>& InSpawnedPaths)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	int32 OverlapCount = 0;
	TArray<AActor*> SpawnedActors;
	for (const TWeakObjectPtr<AActor>& SpawnedActorRef : SpawnedActorRefs)
	{
		if (AActor* SpawnedActor = SpawnedActorRef.Get())
		{
			SpawnedActors.Add(SpawnedActor);
		}
	}

	for (int32 LeftIndex = 0; LeftIndex < SpawnedActors.Num(); ++LeftIndex)
	{
		if (!HasMeaningfulBounds(SpawnedActors[LeftIndex]))
		{
			continue;
		}

		const FBox LeftBounds = ComputeActorBoundsBox(SpawnedActors[LeftIndex]);
		for (int32 RightIndex = LeftIndex + 1; RightIndex < SpawnedActors.Num(); ++RightIndex)
		{
			if (!HasMeaningfulBounds(SpawnedActors[RightIndex]))
			{
				continue;
			}

			const FBox RightBounds = ComputeActorBoundsBox(SpawnedActors[RightIndex]);
			if (LeftBounds.Intersect(RightBounds))
			{
				++OverlapCount;
				LogMessage(FString::Printf(
					TEXT("[AgentBridge L3] Overlap detected: %s <-> %s"),
					*SpawnedActors[LeftIndex]->GetPathName(),
					*SpawnedActors[RightIndex]->GetPathName()));
			}
		}
	}

	LogMessage(FString::Printf(
		TEXT("[AgentBridge L3] Global validation complete (PIE direct): overlaps=%d, spawned=%d, tracked_paths=%d"),
		OverlapCount,
		SpawnedActors.Num(),
		InSpawnedPaths.Num()));
}

void AAgentBridgeFunctionalTest::LogTestReport(int32 TotalActors, int32 PassedActors, int32 FailedActors)
{
	LogMessage(TEXT("========================================"));
	LogMessage(TEXT("[AgentBridge L3] Functional Test Report"));
	LogMessage(FString::Printf(TEXT("[AgentBridge L3] Total actors: %d"), TotalActors));
	LogMessage(FString::Printf(TEXT("[AgentBridge L3] Passed: %d"), PassedActors));
	LogMessage(FString::Printf(TEXT("[AgentBridge L3] Failed: %d"), FailedActors));
	LogMessage(FString::Printf(TEXT("[AgentBridge L3] Tolerance: loc=%.3f rot=%.3f scale=%.4f"),
		LocationTolerance,
		RotationTolerance,
		ScaleTolerance));
	LogMessage(TEXT("========================================"));
}

int32 AAgentBridgeFunctionalTest::GetCommittedTransactionDepth() const
{
	if (!GEditor || !GEditor->Trans)
	{
		return 0;
	}

	return GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
}

FString AAgentBridgeFunctionalTest::BuildFinishedMessage(const FString& CoreMessage) const
{
	TArray<FString> Parts;
	if (!CoreMessage.IsEmpty())
	{
		Parts.Add(CoreMessage);
	}
	if (!LastReportPath.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("Report=%s"), *LastReportPath));
	}
	Parts.Add(FString::Printf(TEXT("Spawned=%d"), SpawnedActorPaths.Num()));
	return FString::Join(Parts, TEXT(" | "));
}
