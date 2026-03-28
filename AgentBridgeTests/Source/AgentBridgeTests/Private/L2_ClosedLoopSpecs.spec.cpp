// L2_ClosedLoopSpecs.spec.cpp
// 目标：实现 3 个 L2 BDD 闭环验证（写 -> 读回 -> 容差比对 -> Undo -> 再读回）。

#include "Misc/AutomationTest.h"
#include "AgentBridgeSubsystem.h"
#include "BridgeTypes.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
// L2 统一容差标准（与文档保持一致）。
const float LocationTolerance = 0.01f;   // cm
const float RotationTolerance = 0.01f;   // degrees
const float ScaleTolerance = 0.001f;     // scale

const TCHAR* StaticMeshActorClass = TEXT("/Script/Engine.StaticMeshActor");
const TCHAR* BoundsTestActorClass = TEXT("/Script/Engine.TriggerBox");

// 获取 L2 Spec 专用 Subsystem，避免 Unity Build 下与 L1_QueryTests 的同名辅助函数冲突。
static UAgentBridgeSubsystem* GetL2SpecSubsystem(FAutomationTestBase& Test)
{
	UAgentBridgeSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>() : nullptr;
	if (!Subsystem)
	{
		Test.AddError(TEXT("AgentBridgeSubsystem 不可用，请确认 AgentBridge 插件已启用且 Editor 已就绪。"));
	}
	return Subsystem;
}

static FString GetCurrentLevelPath()
{
	if (GEditor)
	{
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			return World->GetPathName();
		}
	}
	return TEXT("/Game/Maps/TestMap");
}

static bool TryReadArray3(
	FAutomationTestBase& Test,
	const TSharedPtr<FJsonObject>& JsonObject,
	const TCHAR* FieldName,
	double& OutA,
	double& OutB,
	double& OutC)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!JsonObject.IsValid() || !JsonObject->TryGetArrayField(FieldName, Values) || !Values || Values->Num() != 3)
	{
		Test.AddError(FString::Printf(TEXT("字段 %s 缺失或格式错误（应为 3 元数组）。"), FieldName));
		return false;
	}

	OutA = (*Values)[0].IsValid() ? (*Values)[0]->AsNumber() : 0.0;
	OutB = (*Values)[1].IsValid() ? (*Values)[1]->AsNumber() : 0.0;
	OutC = (*Values)[2].IsValid() ? (*Values)[2]->AsNumber() : 0.0;
	return true;
}

static bool TryGetObjectField(
	FAutomationTestBase& Test,
	const TSharedPtr<FJsonObject>& Parent,
	const TCHAR* FieldName,
	TSharedPtr<FJsonObject>& OutObject)
{
	const TSharedPtr<FJsonObject>* Child = nullptr;
	if (!Parent.IsValid() || !Parent->TryGetObjectField(FieldName, Child) || !Child || !(*Child).IsValid())
	{
		Test.AddError(FString::Printf(TEXT("字段 %s 缺失或不是对象。"), FieldName));
		return false;
	}

	OutObject = *Child;
	return true;
}

static FString ExtractCreatedActorPath(const FBridgeResponse& Response)
{
	if (!Response.Data.IsValid())
	{
		return FString();
	}

	const TArray<TSharedPtr<FJsonValue>>* CreatedObjects = nullptr;
	if (!Response.Data->TryGetArrayField(TEXT("created_objects"), CreatedObjects) || !CreatedObjects || CreatedObjects->Num() == 0)
	{
		return FString();
	}

	const TSharedPtr<FJsonObject> FirstObj = (*CreatedObjects)[0].IsValid() ? (*CreatedObjects)[0]->AsObject() : nullptr;
	if (!FirstObj.IsValid() || !FirstObj->HasTypedField<EJson::String>(TEXT("actor_path")))
	{
		return FString();
	}

	return FirstObj->GetStringField(TEXT("actor_path"));
}

static TArray<FString> ExtractCreatedAssetPaths(const FBridgeResponse& Response)
{
	TArray<FString> AssetPaths;

	if (!Response.Data.IsValid())
	{
		return AssetPaths;
	}

	const TArray<TSharedPtr<FJsonValue>>* CreatedObjects = nullptr;
	if (!Response.Data->TryGetArrayField(TEXT("created_objects"), CreatedObjects) || !CreatedObjects)
	{
		return AssetPaths;
	}

	for (const TSharedPtr<FJsonValue>& Item : *CreatedObjects)
	{
		const TSharedPtr<FJsonObject> Obj = Item.IsValid() ? Item->AsObject() : nullptr;
		if (Obj.IsValid() && Obj->HasTypedField<EJson::String>(TEXT("asset_path")))
		{
			AssetPaths.Add(Obj->GetStringField(TEXT("asset_path")));
		}
	}

	return AssetPaths;
}

static FString JsonObjectToString(const TSharedPtr<FJsonObject>& JsonObject)
{
	if (!JsonObject.IsValid())
	{
		return TEXT("{}");
	}

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return Json;
}

static FString ToPackagePath(const FString& AssetPath)
{
	FString PackagePath;
	FString ObjectName;
	if (AssetPath.Split(TEXT("."), &PackagePath, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromStart))
	{
		return PackagePath;
	}
	return AssetPath;
}

static bool DirtyListContainsImportedAsset(
	const TArray<TSharedPtr<FJsonValue>>& DirtyAssets,
	const TArray<FString>& ImportedAssetPaths)
{
	for (const TSharedPtr<FJsonValue>& DirtyItem : DirtyAssets)
	{
		if (!DirtyItem.IsValid())
		{
			continue;
		}

		const FString DirtyPath = DirtyItem->AsString();
		for (const FString& ImportedPath : ImportedAssetPaths)
		{
			const FString ImportedPackagePath = ToPackagePath(ImportedPath);
			if (DirtyPath.Equals(ImportedPath, ESearchCase::IgnoreCase)
				|| DirtyPath.Equals(ImportedPackagePath, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
	}

	return false;
}

}

BEGIN_DEFINE_SPEC(
	FBridgeL2_SpawnReadbackLoop,
	"Project.AgentBridge.L2.SpawnReadbackLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	UAgentBridgeSubsystem* Subsystem;
	FString LevelPath;
	FString SpawnedActorPath;
	FBridgeTransform InputTransform;
	bool bSpawnSucceeded;
END_DEFINE_SPEC(FBridgeL2_SpawnReadbackLoop)

void FBridgeL2_SpawnReadbackLoop::Define()
{
	Describe("spawn actor then readback via GetActorState", [this]()
	{
		BeforeEach([this]()
		{
			Subsystem = GetL2SpecSubsystem(*this);
			LevelPath = GetCurrentLevelPath();
			SpawnedActorPath.Empty();
			bSpawnSucceeded = false;

			if (!Subsystem)
			{
				return;
			}

			// 固定输入值，后续所有 It 都以它作为容差比对基准。
			InputTransform.Location = FVector(1234.0f, 5678.0f, 90.0f);
			InputTransform.Rotation = FRotator(0.0f, 45.0f, 0.0f);
			InputTransform.RelativeScale3D = FVector(1.5f, 1.5f, 1.5f);

			const FBridgeResponse SpawnResp = Subsystem->SpawnActor(
				LevelPath,
				BoundsTestActorClass,
				TEXT("L2_SpawnTest"),
				InputTransform);

			if (!SpawnResp.IsSuccess())
			{
				AddError(FString::Printf(TEXT("SpawnActor 失败：%s"), *SpawnResp.Summary));
				return;
			}

			SpawnedActorPath = ExtractCreatedActorPath(SpawnResp);
			if (SpawnedActorPath.IsEmpty())
			{
				AddError(TEXT("SpawnActor 成功但未返回 created_objects[0].actor_path。"));
				return;
			}

			bSpawnSucceeded = true;
		});

		It("should return matching location on readback", [this]()
		{
			if (!bSpawnSucceeded)
			{
				AddWarning(TEXT("前置 Spawn 失败，跳过 location 读回验证。"));
				return;
			}

			const FBridgeResponse StateResp = Subsystem->GetActorState(SpawnedActorPath);
			TestTrue(TEXT("GetActorState 应成功"), StateResp.IsSuccess());
			if (!StateResp.IsSuccess() || !StateResp.Data.IsValid())
			{
				return;
			}

			TSharedPtr<FJsonObject> TransformObj;
			if (!TryGetObjectField(*this, StateResp.Data, TEXT("transform"), TransformObj))
			{
				return;
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!TryReadArray3(*this, TransformObj, TEXT("location"), X, Y, Z))
			{
				return;
			}

			TestNearlyEqual(TEXT("Location.X"), static_cast<float>(X), 1234.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Location.Y"), static_cast<float>(Y), 5678.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Location.Z"), static_cast<float>(Z), 90.0f, LocationTolerance);
		});

		It("should return matching rotation on readback", [this]()
		{
			if (!bSpawnSucceeded)
			{
				AddWarning(TEXT("前置 Spawn 失败，跳过 rotation 读回验证。"));
				return;
			}

			const FBridgeResponse StateResp = Subsystem->GetActorState(SpawnedActorPath);
			TestTrue(TEXT("GetActorState 应成功"), StateResp.IsSuccess());
			if (!StateResp.IsSuccess() || !StateResp.Data.IsValid())
			{
				return;
			}

			TSharedPtr<FJsonObject> TransformObj;
			if (!TryGetObjectField(*this, StateResp.Data, TEXT("transform"), TransformObj))
			{
				return;
			}

			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			if (!TryReadArray3(*this, TransformObj, TEXT("rotation"), Pitch, Yaw, Roll))
			{
				return;
			}

			TestNearlyEqual(TEXT("Rotation.Pitch"), static_cast<float>(Pitch), 0.0f, RotationTolerance);
			TestNearlyEqual(TEXT("Rotation.Yaw"), static_cast<float>(Yaw), 45.0f, RotationTolerance);
			TestNearlyEqual(TEXT("Rotation.Roll"), static_cast<float>(Roll), 0.0f, RotationTolerance);
		});

		It("should return matching scale on readback", [this]()
		{
			if (!bSpawnSucceeded)
			{
				AddWarning(TEXT("前置 Spawn 失败，跳过 scale 读回验证。"));
				return;
			}

			const FBridgeResponse StateResp = Subsystem->GetActorState(SpawnedActorPath);
			TestTrue(TEXT("GetActorState 应成功"), StateResp.IsSuccess());
			if (!StateResp.IsSuccess() || !StateResp.Data.IsValid())
			{
				return;
			}

			TSharedPtr<FJsonObject> TransformObj;
			if (!TryGetObjectField(*this, StateResp.Data, TEXT("transform"), TransformObj))
			{
				return;
			}

			double SX = 0.0;
			double SY = 0.0;
			double SZ = 0.0;
			if (!TryReadArray3(*this, TransformObj, TEXT("relative_scale3d"), SX, SY, SZ))
			{
				return;
			}

			TestNearlyEqual(TEXT("Scale.X"), static_cast<float>(SX), 1.5f, ScaleTolerance);
			TestNearlyEqual(TEXT("Scale.Y"), static_cast<float>(SY), 1.5f, ScaleTolerance);
			TestNearlyEqual(TEXT("Scale.Z"), static_cast<float>(SZ), 1.5f, ScaleTolerance);
		});

		It("should be visible in GetActorBounds", [this]()
		{
			if (!bSpawnSucceeded)
			{
				AddWarning(TEXT("前置 Spawn 失败，跳过 bounds 验证。"));
				return;
			}

			const FBridgeResponse BoundsResp = Subsystem->GetActorBounds(SpawnedActorPath);
			TestTrue(TEXT("GetActorBounds 应成功"), BoundsResp.IsSuccess());
			if (!BoundsResp.IsSuccess() || !BoundsResp.Data.IsValid())
			{
				return;
			}

			double OriginX = 0.0;
			double OriginY = 0.0;
			double OriginZ = 0.0;
			double ExtentX = 0.0;
			double ExtentY = 0.0;
			double ExtentZ = 0.0;

			TestTrue(TEXT("应包含 world_bounds_origin[3]"),
				TryReadArray3(*this, BoundsResp.Data, TEXT("world_bounds_origin"), OriginX, OriginY, OriginZ));
			TestTrue(TEXT("应包含 world_bounds_extent[3]"),
				TryReadArray3(*this, BoundsResp.Data, TEXT("world_bounds_extent"), ExtentX, ExtentY, ExtentZ));

			const bool bExtentNonZero =
				!FMath::IsNearlyZero(static_cast<float>(ExtentX))
				|| !FMath::IsNearlyZero(static_cast<float>(ExtentY))
				|| !FMath::IsNearlyZero(static_cast<float>(ExtentZ));
			TestTrue(TEXT("world_bounds_extent 应非零（Actor 应有物理尺寸）"), bExtentNonZero);
		});

		It("should mark level as dirty", [this]()
		{
			if (!bSpawnSucceeded)
			{
				AddWarning(TEXT("前置 Spawn 失败，跳过 dirty_assets 验证。"));
				return;
			}

			const FBridgeResponse DirtyResp = Subsystem->GetDirtyAssets();
			TestTrue(TEXT("GetDirtyAssets 应成功"), DirtyResp.IsSuccess());
			if (!DirtyResp.IsSuccess() || !DirtyResp.Data.IsValid())
			{
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* DirtyAssets = nullptr;
			TestTrue(TEXT("dirty_assets 应为数组"), DirtyResp.Data->TryGetArrayField(TEXT("dirty_assets"), DirtyAssets) && DirtyAssets != nullptr);
			if (DirtyAssets)
			{
				TestTrue(TEXT("dirty_assets 应非空"), DirtyAssets->Num() > 0);
			}
		});

		AfterEach([this]()
		{
			// 每个 It 结束后回滚 Spawn，确保关卡无残留。
			if (bSpawnSucceeded && GEditor)
			{
				GEditor->UndoTransaction();
			}
			bSpawnSucceeded = false;
			SpawnedActorPath.Empty();
		});
	});
}

BEGIN_DEFINE_SPEC(
	FBridgeL2_TransformModifyLoop,
	"Project.AgentBridge.L2.TransformModifyLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	UAgentBridgeSubsystem* Subsystem;
	FString LevelPath;
	FString ActorPath;
	FBridgeTransform OriginalTransform;
	FBridgeTransform NewTransform;
	bool bSpawnSucceeded;
	bool bTransformApplied;
END_DEFINE_SPEC(FBridgeL2_TransformModifyLoop)

void FBridgeL2_TransformModifyLoop::Define()
{
	Describe("modify transform then verify readback", [this]()
	{
		BeforeEach([this]()
		{
			Subsystem = GetL2SpecSubsystem(*this);
			LevelPath = GetCurrentLevelPath();
			ActorPath.Empty();
			bSpawnSucceeded = false;
			bTransformApplied = false;

			if (!Subsystem)
			{
				return;
			}

			OriginalTransform.Location = FVector(200.0f, 300.0f, 0.0f);
			OriginalTransform.Rotation = FRotator(0.0f, 0.0f, 0.0f);
			OriginalTransform.RelativeScale3D = FVector(1.0f, 1.0f, 1.0f);

			NewTransform.Location = FVector(800.0f, 900.0f, 50.0f);
			NewTransform.Rotation = FRotator(0.0f, 90.0f, 0.0f);
			NewTransform.RelativeScale3D = FVector(2.0f, 2.0f, 2.0f);

			const FBridgeResponse SpawnResp = Subsystem->SpawnActor(
				LevelPath,
				StaticMeshActorClass,
				TEXT("L2_TransformTest"),
				OriginalTransform);

			if (!SpawnResp.IsSuccess())
			{
				AddError(FString::Printf(TEXT("TransformModifyLoop 前置 Spawn 失败：%s"), *SpawnResp.Summary));
				return;
			}

			ActorPath = ExtractCreatedActorPath(SpawnResp);
			if (ActorPath.IsEmpty())
			{
				AddError(TEXT("TransformModifyLoop 未获取到 actor_path。"));
				return;
			}

			bSpawnSucceeded = true;
		});

		It("should return old_transform matching original", [this]()
		{
			if (!bSpawnSucceeded)
			{
				AddWarning(TEXT("前置 Spawn 失败，跳过 old_transform 验证。"));
				return;
			}

			const FBridgeResponse SetResp = Subsystem->SetActorTransform(ActorPath, NewTransform);
			bTransformApplied = SetResp.IsSuccess();

			TestTrue(TEXT("SetActorTransform 应成功"), SetResp.IsSuccess());
			if (!SetResp.IsSuccess() || !SetResp.Data.IsValid())
			{
				return;
			}

			TSharedPtr<FJsonObject> OldTransformObj;
			TSharedPtr<FJsonObject> ActualTransformObj;
			if (!TryGetObjectField(*this, SetResp.Data, TEXT("old_transform"), OldTransformObj))
			{
				return;
			}
			if (!TryGetObjectField(*this, SetResp.Data, TEXT("actual_transform"), ActualTransformObj))
			{
				return;
			}

			double OldX = 0.0;
			double OldY = 0.0;
			double OldZ = 0.0;
			if (!TryReadArray3(*this, OldTransformObj, TEXT("location"), OldX, OldY, OldZ))
			{
				return;
			}

			TestNearlyEqual(TEXT("Old Location.X"), static_cast<float>(OldX), 200.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Old Location.Y"), static_cast<float>(OldY), 300.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Old Location.Z"), static_cast<float>(OldZ), 0.0f, LocationTolerance);

			// old_transform 必须是修改前快照，不应与 actual_transform 完全一致。
			const FString OldJson = JsonObjectToString(OldTransformObj);
			const FString ActualJson = JsonObjectToString(ActualTransformObj);
			TestTrue(TEXT("old_transform 应与 actual_transform 不同"), OldJson != ActualJson);
		});

		It("should readback modified values via GetActorState", [this]()
		{
			if (!bSpawnSucceeded)
			{
				AddWarning(TEXT("前置 Spawn 失败，跳过 modified readback 验证。"));
				return;
			}

			const FBridgeResponse SetResp = Subsystem->SetActorTransform(ActorPath, NewTransform);
			bTransformApplied = SetResp.IsSuccess();
			TestTrue(TEXT("SetActorTransform 应成功"), SetResp.IsSuccess());
			if (!SetResp.IsSuccess())
			{
				return;
			}

			const FBridgeResponse StateResp = Subsystem->GetActorState(ActorPath);
			TestTrue(TEXT("GetActorState 应成功"), StateResp.IsSuccess());
			if (!StateResp.IsSuccess() || !StateResp.Data.IsValid())
			{
				return;
			}

			TSharedPtr<FJsonObject> TransformObj;
			if (!TryGetObjectField(*this, StateResp.Data, TEXT("transform"), TransformObj))
			{
				return;
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			double SX = 0.0;
			double SY = 0.0;
			double SZ = 0.0;

			if (!TryReadArray3(*this, TransformObj, TEXT("location"), X, Y, Z))
			{
				return;
			}
			if (!TryReadArray3(*this, TransformObj, TEXT("rotation"), Pitch, Yaw, Roll))
			{
				return;
			}
			if (!TryReadArray3(*this, TransformObj, TEXT("relative_scale3d"), SX, SY, SZ))
			{
				return;
			}

			TestNearlyEqual(TEXT("Modified X"), static_cast<float>(X), 800.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Modified Y"), static_cast<float>(Y), 900.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Modified Z"), static_cast<float>(Z), 50.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Modified Yaw"), static_cast<float>(Yaw), 90.0f, RotationTolerance);
			TestNearlyEqual(TEXT("Modified Scale.X"), static_cast<float>(SX), 2.0f, ScaleTolerance);
			TestNearlyEqual(TEXT("Modified Scale.Y"), static_cast<float>(SY), 2.0f, ScaleTolerance);
			TestNearlyEqual(TEXT("Modified Scale.Z"), static_cast<float>(SZ), 2.0f, ScaleTolerance);
		});

		It("should be undoable via Transaction", [this]()
		{
			if (!bSpawnSucceeded)
			{
				AddWarning(TEXT("前置 Spawn 失败，跳过 Undo 验证。"));
				return;
			}

			const FBridgeResponse SetResp = Subsystem->SetActorTransform(ActorPath, NewTransform);
			bTransformApplied = SetResp.IsSuccess();
			TestTrue(TEXT("SetActorTransform 应成功"), SetResp.IsSuccess());
			if (!SetResp.IsSuccess())
			{
				return;
			}

			if (!GEditor)
			{
				AddError(TEXT("GEditor 不可用，无法执行 Undo 回滚验证。"));
				return;
			}

			// 回滚 SetActorTransform，并验证读回恢复到 OriginalTransform。
			GEditor->UndoTransaction();
			bTransformApplied = false;

			const FBridgeResponse StateResp = Subsystem->GetActorState(ActorPath);
			TestTrue(TEXT("Undo 后 GetActorState 应成功"), StateResp.IsSuccess());
			if (!StateResp.IsSuccess() || !StateResp.Data.IsValid())
			{
				return;
			}

			TSharedPtr<FJsonObject> TransformObj;
			if (!TryGetObjectField(*this, StateResp.Data, TEXT("transform"), TransformObj))
			{
				return;
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!TryReadArray3(*this, TransformObj, TEXT("location"), X, Y, Z))
			{
				return;
			}

			TestNearlyEqual(TEXT("Undone X"), static_cast<float>(X), 200.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Undone Y"), static_cast<float>(Y), 300.0f, LocationTolerance);
			TestNearlyEqual(TEXT("Undone Z"), static_cast<float>(Z), 0.0f, LocationTolerance);
		});

		AfterEach([this]()
		{
			// 若 It 中未回滚 Transform，则先回滚 Transform；再回滚 Spawn。
			if (GEditor && bTransformApplied)
			{
				GEditor->UndoTransaction();
				bTransformApplied = false;
			}
			if (GEditor && bSpawnSucceeded)
			{
				GEditor->UndoTransaction();
				bSpawnSucceeded = false;
			}
			ActorPath.Empty();
		});
	});
}

BEGIN_DEFINE_SPEC(
	FBridgeL2_ImportMetadataLoop,
	"Project.AgentBridge.L2.ImportMetadataLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	UAgentBridgeSubsystem* Subsystem;
	FString TestSourceDir;
	FString TestDestPath;
	bool bHasTestAssets;
	bool bImportApplied;
	TArray<FString> ImportedAssetPaths;
END_DEFINE_SPEC(FBridgeL2_ImportMetadataLoop)

void FBridgeL2_ImportMetadataLoop::Define()
{
	Describe("import assets then verify via GetAssetMetadata", [this]()
	{
		BeforeEach([this]()
		{
			Subsystem = GetL2SpecSubsystem(*this);
			TestSourceDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("TestResources/ImportTest"));
			TestDestPath = TEXT("/Game/Tests/L2_ImportTest");
			bHasTestAssets = false;
			bImportApplied = false;
			ImportedAssetPaths.Reset();

			if (!Subsystem)
			{
				return;
			}

			// CI 环境可能不提供测试资源目录：按 WARNING + 跳过处理。
			if (!FPaths::DirectoryExists(TestSourceDir))
			{
				return;
			}

			const FBridgeResponse ImportResp = Subsystem->ImportAssets(TestSourceDir, TestDestPath, true);
			if (!ImportResp.IsSuccess())
			{
				AddError(FString::Printf(TEXT("ImportAssets 失败：%s"), *ImportResp.Summary));
				return;
			}

			bImportApplied = true;
			ImportedAssetPaths = ExtractCreatedAssetPaths(ImportResp);

			if (ImportedAssetPaths.Num() == 0)
			{
				AddWarning(FString::Printf(TEXT("目录 %s 存在，但没有可导入测试资源，后续 It 将跳过。"), *TestSourceDir));
				return;
			}

			bHasTestAssets = true;
		});

		It("should find imported asset via GetAssetMetadata", [this]()
		{
			if (!bHasTestAssets)
			{
				AddWarning(FString::Printf(TEXT("No test assets: %s"), *TestSourceDir));
				return;
			}

			const FString& ImportedAssetPath = ImportedAssetPaths[0];
			const FBridgeResponse MetaResp = Subsystem->GetAssetMetadata(ImportedAssetPath);
			TestTrue(TEXT("GetAssetMetadata 应成功"), MetaResp.IsSuccess());
			if (!MetaResp.IsSuccess() || !MetaResp.Data.IsValid())
			{
				return;
			}

			TestTrue(TEXT("exists 应为 true"), MetaResp.Data->HasField(TEXT("exists")) && MetaResp.Data->GetBoolField(TEXT("exists")));
			TestTrue(TEXT("class 应非空"),
				MetaResp.Data->HasField(TEXT("class")) && !MetaResp.Data->GetStringField(TEXT("class")).IsEmpty());
		});

		It("should list imported assets as dirty", [this]()
		{
			if (!bHasTestAssets)
			{
				AddWarning(FString::Printf(TEXT("No test assets: %s"), *TestSourceDir));
				return;
			}

			const FBridgeResponse DirtyResp = Subsystem->GetDirtyAssets();
			TestTrue(TEXT("GetDirtyAssets 应成功"), DirtyResp.IsSuccess());
			if (!DirtyResp.IsSuccess() || !DirtyResp.Data.IsValid())
			{
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* DirtyAssets = nullptr;
			TestTrue(TEXT("dirty_assets 应为数组"), DirtyResp.Data->TryGetArrayField(TEXT("dirty_assets"), DirtyAssets) && DirtyAssets != nullptr);
			if (!DirtyAssets)
			{
				return;
			}

			const bool bContainsImported = DirtyListContainsImportedAsset(*DirtyAssets, ImportedAssetPaths);
			TestTrue(TEXT("dirty_assets 应包含导入资源路径（对象路径或包路径）"), bContainsImported);
		});

		AfterEach([this]()
		{
			if (bImportApplied && GEditor)
			{
				GEditor->UndoTransaction();
			}
			bImportApplied = false;
			bHasTestAssets = false;
			ImportedAssetPaths.Reset();
		});
	});
}

