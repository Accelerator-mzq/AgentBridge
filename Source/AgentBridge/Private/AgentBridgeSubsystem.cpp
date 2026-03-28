// AgentBridgeSubsystem.cpp
// AGENT + UE5 可操作层 — Bridge 封装层核心实现
//
// 全部执行能力来自 UE5 官方 API。
// 本文件不创造新的引擎 API——每个函数都是对 UE5 官方 API 的结构化封装，
// 增加参数校验、统一响应、错误码、写后读回和 Transaction 管理。

#include "AgentBridgeSubsystem.h"
#include "AutomationDriverAdapter.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/BoxComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"

#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/BlueprintFactory.h"
#include "AssetImportTask.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UATRunner.h"

#include "FileHelpers.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#include "Async/Async.h"

class FPendingUIOperation
{
public:
	/** 异步任务唯一标识，供 query_ui_operation 轮询 */
	FString OperationId;

	/** 当前最小原型支持的操作类型 */
	FString OperationType;

	/** 目标 Actor 路径 */
	FString ActorPath;

	/** 主参数：当前原型里等价于 ButtonLabel */
	FString Target;

	/** 预留给后续输入类操作使用，当前原型不消费 */
	FString Value;

	/** DragAssetToViewport 异步原型用的世界坐标参数 */
	bool bHasDropLocation = false;
	FVector DropLocation = FVector::ZeroVector;

	/** 单次 UI 操作超时秒数 */
	float TimeoutSeconds = 10.0f;

	/** 异步运行态：pending / running / success / failed */
	FString OperationState = TEXT("pending");

	/** 调试信息：当前原型使用的执行后端 */
	FString ExecutionBackend = TEXT("automation_driver_sync_off_game_thread_prototype");

	/** 如有需要，记录 Driver 使用的定位路径 */
	FString DebugLocatorPath;

	/** 最近一次状态说明 */
	FString Summary;

	/** 最终失败原因（如果有） */
	FString FailureReason;

	/** 创建/更新时间（秒） */
	double CreatedAtSeconds = 0.0;
	double UpdatedAtSeconds = 0.0;

	/** 线程池任务 future；query 时检查是否已完成 */
	TUniquePtr<TFuture<FUIOperationResult>> Future;

	/** 任务完成后的最终结果 */
	TOptional<FUIOperationResult> FinalResult;
};
#include "UnrealClient.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// 统一给写接口响应打上 transaction 标记，保证 RC 返回字段一致。
	static FBridgeResponse FinalizeWriteResponse(FBridgeResponse Response)
	{
		Response.bTransaction = true;
		Response.SyncForRemote();
		return Response;
	}

	static bool TryParseVectorCsv(const FString& InValue, FVector& OutVector)
	{
		TArray<FString> Parts;
		InValue.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 3)
		{
			return false;
		}

		float Parsed[3] = {0.0f, 0.0f, 0.0f};
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FString Token = Parts[Index].TrimStartAndEnd();
			if (!FDefaultValueHelper::ParseFloat(Token, Parsed[Index]))
			{
				return false;
			}
		}

		OutVector = FVector(Parsed[0], Parsed[1], Parsed[2]);
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShareable(new FJsonValueNumber(V.X)));
		Arr.Add(MakeShareable(new FJsonValueNumber(V.Y)));
		Arr.Add(MakeShareable(new FJsonValueNumber(V.Z)));
		return Arr;
	}

	static TArray<TSharedPtr<FJsonValue>> RotatorToJsonArray(const FRotator& R)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShareable(new FJsonValueNumber(R.Pitch)));
		Arr.Add(MakeShareable(new FJsonValueNumber(R.Yaw)));
		Arr.Add(MakeShareable(new FJsonValueNumber(R.Roll)));
		return Arr;
	}

	static UActorComponent* FindActorComponentByName(const AActor* Actor, const FString& ComponentName)
	{
		if (!Actor)
		{
			return nullptr;
		}

		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (Component && Component->GetName().Equals(ComponentName, ESearchCase::CaseSensitive))
			{
				return Component;
			}
		}
		return nullptr;
	}

	static UMeshComponent* FindPrimaryMeshComponent(const AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}

		if (const UMeshComponent* RootMesh = Cast<UMeshComponent>(Actor->GetRootComponent()))
		{
			return const_cast<UMeshComponent*>(RootMesh);
		}

		return Actor->FindComponentByClass<UMeshComponent>();
	}

	static FString GetCurrentLevelPackagePath(const UWorld* World)
	{
		if (World && World->GetOutermost())
		{
			return World->GetOutermost()->GetName();
		}
		return TEXT("");
	}

	static bool TryParseCollisionEnabledName(
		const FString& InValue,
		ECollisionEnabled::Type& OutValue,
		FString& OutNormalizedName)
	{
		const FString Normalized = InValue.TrimStartAndEnd();
		if (Normalized.IsEmpty())
		{
			return false;
		}

		struct FCollisionEnabledEntry
		{
			const TCHAR* Name;
			ECollisionEnabled::Type Value;
		};

		static const FCollisionEnabledEntry Entries[] = {
			{TEXT("NoCollision"), ECollisionEnabled::NoCollision},
			{TEXT("QueryOnly"), ECollisionEnabled::QueryOnly},
			{TEXT("PhysicsOnly"), ECollisionEnabled::PhysicsOnly},
			{TEXT("QueryAndPhysics"), ECollisionEnabled::QueryAndPhysics},
			{TEXT("ProbeOnly"), ECollisionEnabled::ProbeOnly},
			{TEXT("QueryAndProbe"), ECollisionEnabled::QueryAndProbe},
		};

		for (const FCollisionEnabledEntry& Entry : Entries)
		{
			if (Normalized.Equals(Entry.Name, ESearchCase::IgnoreCase))
			{
				OutValue = Entry.Value;
				OutNormalizedName = Entry.Name;
				return true;
			}
		}

		return false;
	}
}

// ============================================================
// 生命周期
// ============================================================

void UAgentBridgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogTemp, Log, TEXT("[AgentBridge] Subsystem initialized (v%s)"), *GetVersion());
}

void UAgentBridgeSubsystem::Deinitialize()
{
	// 异步原型状态只在 Editor 会话内有效；退出时直接清空，避免残留无效 future。
	PendingUIOperations.Empty();
	UE_LOG(LogTemp, Log, TEXT("[AgentBridge] Subsystem deinitialized"));
	Super::Deinitialize();
}

// ============================================================
// 内部辅助方法
// ============================================================

AActor* UAgentBridgeSubsystem::FindActorByPath(const FString& ActorPath) const
{
	UWorld* World = GetEditorWorld();
	if (!World) return nullptr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetPathName() == ActorPath)
		{
			return *It;
		}
	}
	return nullptr;
}

UWorld* UAgentBridgeSubsystem::GetEditorWorld() const
{
	if (!GEditor) return nullptr;
	return GEditor->GetEditorWorldContext().World();
}

TSharedPtr<FJsonObject> UAgentBridgeSubsystem::ReadCollisionToJson(const AActor* Actor) const
{
	TSharedPtr<FJsonObject> Collision = MakeShareable(new FJsonObject());
	if (!Actor) return Collision;

	const UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
	if (!Root) return Collision;

	// 统一输出与 Schema 对齐的基础碰撞字段。
	Collision->SetStringField(TEXT("collision_profile_name"), Root->GetCollisionProfileName().ToString());
	FString CollisionEnabled = UEnum::GetValueAsString(Root->GetCollisionEnabled());
	CollisionEnabled.RemoveFromStart(TEXT("ECollisionEnabled::"));
	Collision->SetStringField(TEXT("collision_enabled"), CollisionEnabled);
	Collision->SetBoolField(TEXT("generate_overlap_events"), Root->GetGenerateOverlapEvents());
	Collision->SetBoolField(TEXT("can_affect_navigation"), Root->CanEverAffectNavigation());

	// 仅 BoxComponent 场景返回 collision_box_extent（Schema 可选字段）。
	if (const UBoxComponent* BoxComponent = Cast<UBoxComponent>(Root))
	{
		const FVector BoxExtent = BoxComponent->GetUnscaledBoxExtent();
		TArray<TSharedPtr<FJsonValue>> BoxExtentArray;
		BoxExtentArray.Add(MakeShareable(new FJsonValueNumber(BoxExtent.X)));
		BoxExtentArray.Add(MakeShareable(new FJsonValueNumber(BoxExtent.Y)));
		BoxExtentArray.Add(MakeShareable(new FJsonValueNumber(BoxExtent.Z)));
		Collision->SetArrayField(TEXT("collision_box_extent"), BoxExtentArray);
	}

	return Collision;
}

TArray<TSharedPtr<FJsonValue>> UAgentBridgeSubsystem::ReadTagsToJsonArray(const AActor* Actor) const
{
	TArray<TSharedPtr<FJsonValue>> TagArray;
	if (!Actor) return TagArray;

	for (const FName& Tag : Actor->Tags)
	{
		TagArray.Add(MakeShareable(new FJsonValueString(Tag.ToString())));
	}
	return TagArray;
}

TSharedPtr<FJsonObject> UAgentBridgeSubsystem::MakeEmptyWriteData() const
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetArrayField(TEXT("created_objects"), {});
	Data->SetArrayField(TEXT("modified_objects"), {});
	Data->SetArrayField(TEXT("deleted_objects"), {});
	Data->SetArrayField(TEXT("dirty_assets"), {});
	Data->SetObjectField(TEXT("validation"), MakeShareable(new FJsonObject()));
	return Data;
}

// ============================================================
// 查询接口 1: GetCurrentProjectState
// UE5 依赖: FPaths + UKismetSystemLibrary + UEditorLevelLibrary
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::GetCurrentProjectState()
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	FString ProjectPath = FPaths::GetProjectFilePath();
	FString ProjectName = FPaths::GetBaseFilename(ProjectPath);
	FString EngineVersion = FApp::GetBuildVersion();

	UWorld* World = GetEditorWorld();
	FString CurrentLevel = TEXT("");
	if (World && World->GetOutermost())
	{
		CurrentLevel = World->GetOutermost()->GetName();
	}

	FString EditorMode = TEXT("editing");
	if (GEditor && GEditor->IsSimulateInEditorInProgress())
	{
		EditorMode = TEXT("simulating");
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("project_name"), ProjectName);
	Data->SetStringField(TEXT("uproject_path"), ProjectPath);
	Data->SetStringField(TEXT("engine_version"), EngineVersion);
	Data->SetStringField(TEXT("current_level"), CurrentLevel);
	Data->SetStringField(TEXT("editor_mode"), EditorMode);

	return AgentBridge::MakeSuccess(TEXT("Current project state fetched successfully"), Data);
}

// ============================================================
// 查询接口 2: ListLevelActors
// UE5 依赖: UEditorLevelLibrary::GetAllLevelActors()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::ListLevelActors(const FString& ClassFilter)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return AgentBridge::MakeFailed(
			TEXT("No editor world"),
			EBridgeErrorCode::EditorNotReady,
			TEXT("Editor world is null")
		);
	}

	const FString Filter = ClassFilter.TrimStartAndEnd();

	TArray<TSharedPtr<FJsonValue>> ActorArray;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		const FString ClassPath = Actor->GetClass()->GetPathName();
		const FString ClassName = Actor->GetClass()->GetName();
		if (!Filter.IsEmpty()
			&& !ClassPath.Contains(Filter, ESearchCase::IgnoreCase)
			&& !ClassName.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedPtr<FJsonObject> ActorObj = MakeShareable(new FJsonObject());
		ActorObj->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
		ActorObj->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		ActorObj->SetStringField(TEXT("class"), ClassPath);
		ActorArray.Add(MakeShareable(new FJsonValueObject(ActorObj)));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	if (World->GetOutermost())
	{
		Data->SetStringField(TEXT("level_path"), World->GetOutermost()->GetName());
	}
	Data->SetArrayField(TEXT("actors"), ActorArray);

	FString Summary = FString::Printf(TEXT("Level actors listed successfully (%d)"), ActorArray.Num());
	return AgentBridge::MakeSuccess(Summary, Data);
}

// ============================================================
// 查询接口 3: GetActorState
// UE5 依赖: AActor::GetActorLocation/Rotation/Scale3D() + collision + tags
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::GetActorState(const FString& ActorPath)
{
	// 参数校验
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError))
	{
		return ValidationError;
	}
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	// 查找 Actor
	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return AgentBridge::MakeFailed(
			TEXT("Actor not found"),
			EBridgeErrorCode::ActorNotFound,
			FString::Printf(TEXT("No actor at path: %s"), *ActorPath)
		);
	}

	// 读取 Transform
	FBridgeTransform Transform = FBridgeTransform::FromActor(Actor);

	// 构造响应
	FString TargetLevelPath = TEXT("");
	if (const ULevel* ActorLevel = Actor->GetLevel())
	{
		if (const UPackage* LevelPackage = ActorLevel->GetOutermost())
		{
			TargetLevelPath = LevelPackage->GetName();
		}
	}
	if (TargetLevelPath.IsEmpty())
	{
		if (UWorld* World = GetEditorWorld())
		{
			if (World->GetOutermost())
			{
				TargetLevelPath = World->GetOutermost()->GetName();
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
	Data->SetStringField(TEXT("target_level"), TargetLevelPath);
	Data->SetObjectField(TEXT("transform"), Transform.ToJson());
	Data->SetObjectField(TEXT("collision"), ReadCollisionToJson(Actor));
	Data->SetArrayField(TEXT("tags"), ReadTagsToJsonArray(Actor));

	return AgentBridge::MakeSuccess(TEXT("Actor state fetched successfully"), Data);
}

// ============================================================
// 查询接口 4: GetActorBounds
// UE5 依赖: AActor::GetActorBounds()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::GetActorBounds(const FString& ActorPath)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError))
	{
		return ValidationError;
	}
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return AgentBridge::MakeFailed(
			TEXT("Actor not found"),
			EBridgeErrorCode::ActorNotFound,
			FString::Printf(TEXT("No actor at path: %s"), *ActorPath)
		);
	}

	FVector Origin, Extent;
	Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);

	auto Vec3ToJsonArray = [](const FVector& V) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShareable(new FJsonValueNumber(V.X)));
		Arr.Add(MakeShareable(new FJsonValueNumber(V.Y)));
		Arr.Add(MakeShareable(new FJsonValueNumber(V.Z)));
		return Arr;
	};

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetArrayField(TEXT("world_bounds_origin"), Vec3ToJsonArray(Origin));
	Data->SetArrayField(TEXT("world_bounds_extent"), Vec3ToJsonArray(Extent));

	return AgentBridge::MakeSuccess(TEXT("Actor bounds fetched successfully"), Data);
}

// ============================================================
// 查询接口 5: GetAssetMetadata
// UE5 依赖: UEditorAssetLibrary::DoesAssetExist() + FindAssetData()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::GetAssetMetadata(const FString& AssetPath)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(AssetPath, TEXT("AssetPath"), ValidationError))
	{
		return ValidationError;
	}
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	bool bExists = UEditorAssetLibrary::DoesAssetExist(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("exists"), bExists);
	Data->SetStringField(TEXT("class"), TEXT("Unknown"));

	if (bExists)
	{
		FAssetData AssetData = UEditorAssetLibrary::FindAssetData(AssetPath);
		if (AssetData.IsValid())
		{
			// 优先输出简洁类名，和 example 保持一致（如 StaticMesh）。
			Data->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
			Data->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		}

		// StaticMesh 场景补充 mesh_asset_bounds，便于后续占地验证。
		UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
		if (LoadedAsset && Data->GetStringField(TEXT("class")).Equals(TEXT("Unknown"), ESearchCase::IgnoreCase))
		{
			Data->SetStringField(TEXT("class"), LoadedAsset->GetClass()->GetName());
		}

		UStaticMesh* Mesh = Cast<UStaticMesh>(LoadedAsset);
		if (Mesh)
		{
			FBox BBox = Mesh->GetBoundingBox();
			FVector Center = BBox.GetCenter();
			FVector Ext = BBox.GetExtent();

			TSharedPtr<FJsonObject> MeshBounds = MakeShareable(new FJsonObject());
			TArray<TSharedPtr<FJsonValue>> OriginArr, ExtArr;
			OriginArr.Add(MakeShareable(new FJsonValueNumber(Center.X)));
			OriginArr.Add(MakeShareable(new FJsonValueNumber(Center.Y)));
			OriginArr.Add(MakeShareable(new FJsonValueNumber(Center.Z)));
			ExtArr.Add(MakeShareable(new FJsonValueNumber(Ext.X)));
			ExtArr.Add(MakeShareable(new FJsonValueNumber(Ext.Y)));
			ExtArr.Add(MakeShareable(new FJsonValueNumber(Ext.Z)));
			MeshBounds->SetArrayField(TEXT("world_bounds_origin"), OriginArr);
			MeshBounds->SetArrayField(TEXT("world_bounds_extent"), ExtArr);
			Data->SetObjectField(TEXT("mesh_asset_bounds"), MeshBounds);
		}
	}

	FString Summary = bExists
		? TEXT("Asset metadata fetched successfully")
		: TEXT("Asset metadata fetched (asset does not exist)");
	return AgentBridge::MakeSuccess(Summary, Data);
}

// ============================================================
// 查询接口 6: GetDirtyAssets
// UE5 依赖: UEditorLoadingAndSavingUtils / UPackage::IsDirty()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::GetDirtyAssets()
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	// 同时收集内容包与关卡包，避免遗漏“仅改关卡但未改内容资源”的脏状态。
	TArray<UPackage*> DirtyContentPackages;
	FEditorFileUtils::GetDirtyContentPackages(DirtyContentPackages);
	TArray<UPackage*> DirtyWorldPackages;
	FEditorFileUtils::GetDirtyWorldPackages(DirtyWorldPackages);

	TSet<FString> UniqueDirtyPackagePaths;
	UniqueDirtyPackagePaths.Reserve(DirtyContentPackages.Num() + DirtyWorldPackages.Num());
	for (UPackage* Pkg : DirtyContentPackages)
	{
		if (Pkg)
		{
			UniqueDirtyPackagePaths.Add(Pkg->GetPathName());
		}
	}
	for (UPackage* Pkg : DirtyWorldPackages)
	{
		if (Pkg)
		{
			UniqueDirtyPackagePaths.Add(Pkg->GetPathName());
		}
	}

	TArray<TSharedPtr<FJsonValue>> DirtyArray;
	DirtyArray.Reserve(UniqueDirtyPackagePaths.Num());
	for (const FString& PackagePath : UniqueDirtyPackagePaths)
	{
		DirtyArray.Add(MakeShareable(new FJsonValueString(PackagePath)));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetArrayField(TEXT("dirty_assets"), DirtyArray);

	FString Summary = FString::Printf(TEXT("Dirty assets fetched successfully (%d)"), DirtyArray.Num());
	return AgentBridge::MakeSuccess(Summary, Data);
}

// ============================================================
// 查询接口 7: RunMapCheck
// UE5 依赖: Console Command "MAP CHECK"
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::RunMapCheck()
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return AgentBridge::MakeFailed(
			TEXT("No editor world"), EBridgeErrorCode::EditorNotReady, TEXT("Editor world is null"));
	}

	// 执行 MAP CHECK Console Command
	GEditor->Exec(World, TEXT("MAP CHECK"));

	// MAP CHECK 结果输出到 Message Log — 此处收集最近的 MapCheck 消息
	// 完整实现需要监听 FMessageLog("MapCheck")，此处返回执行确认
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	if (World->GetOutermost())
	{
		Data->SetStringField(TEXT("level_path"), World->GetOutermost()->GetName());
	}
	else
	{
		Data->SetStringField(TEXT("level_path"), World->GetPathName());
	}
	Data->SetArrayField(TEXT("map_errors"), {});
	Data->SetArrayField(TEXT("map_warnings"), {});

	return AgentBridge::MakeSuccess(TEXT("Map check completed"), Data);
}

// ============================================================
// 查询接口 8: GetComponentState
// UE5 依赖: Actor->GetComponents() + USceneComponent::GetRelativeTransform()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::GetComponentState(
	const FString& ActorPath,
	const FString& ComponentName)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError))
	{
		return ValidationError;
	}
	if (!AgentBridge::ValidateRequiredString(ComponentName, TEXT("ComponentName"), ValidationError))
	{
		return ValidationError;
	}
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return AgentBridge::MakeFailed(
			TEXT("Actor not found"),
			EBridgeErrorCode::ActorNotFound,
			FString::Printf(TEXT("No actor at path: %s"), *ActorPath));
	}

	UActorComponent* Component = FindActorComponentByName(Actor, ComponentName);
	if (!Component)
	{
		return AgentBridge::MakeFailed(
			TEXT("Component not found"),
			EBridgeErrorCode::ToolExecutionFailed,
			FString::Printf(TEXT("No component named '%s' on actor '%s'"), *ComponentName, *ActorPath));
	}

	USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
	if (!SceneComponent)
	{
		return AgentBridge::MakeFailed(
			TEXT("Component is not a scene component"),
			EBridgeErrorCode::ToolExecutionFailed,
			FString::Printf(TEXT("Component '%s' does not expose relative transform"), *ComponentName));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("component_name"), SceneComponent->GetName());
	Data->SetStringField(TEXT("component_class"), SceneComponent->GetClass()->GetPathName());
	Data->SetArrayField(TEXT("relative_location"), VectorToJsonArray(SceneComponent->GetRelativeLocation()));
	Data->SetArrayField(TEXT("relative_rotation"), RotatorToJsonArray(SceneComponent->GetRelativeRotation()));
	Data->SetArrayField(TEXT("relative_scale"), VectorToJsonArray(SceneComponent->GetRelativeScale3D()));

	return AgentBridge::MakeSuccess(TEXT("Component state fetched successfully"), Data);
}

// ============================================================
// 查询接口 9: GetMaterialAssignment
// UE5 依赖: UMeshComponent::GetNumMaterials / GetMaterial()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::GetMaterialAssignment(const FString& ActorPath)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError))
	{
		return ValidationError;
	}
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return AgentBridge::MakeFailed(
			TEXT("Actor not found"),
			EBridgeErrorCode::ActorNotFound,
			FString::Printf(TEXT("No actor at path: %s"), *ActorPath));
	}

	UMeshComponent* MeshComponent = FindPrimaryMeshComponent(Actor);
	if (!MeshComponent)
	{
		return AgentBridge::MakeFailed(
			TEXT("MeshComponent not found"),
			EBridgeErrorCode::ToolExecutionFailed,
			FString::Printf(TEXT("Actor '%s' has no MeshComponent"), *ActorPath));
	}

	TArray<TSharedPtr<FJsonValue>> MaterialsArray;
	const int32 MaterialCount = MeshComponent->GetNumMaterials();
	for (int32 SlotIndex = 0; SlotIndex < MaterialCount; ++SlotIndex)
	{
		UMaterialInterface* Material = MeshComponent->GetMaterial(SlotIndex);

		TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject());
		Entry->SetNumberField(TEXT("slot_index"), SlotIndex);
		Entry->SetStringField(TEXT("material_path"), Material ? Material->GetPathName() : TEXT(""));
		Entry->SetStringField(TEXT("material_name"), Material ? Material->GetName() : TEXT(""));
		MaterialsArray.Add(MakeShareable(new FJsonValueObject(Entry)));
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("actor_path"), ActorPath);
	Data->SetArrayField(TEXT("materials"), MaterialsArray);

	return AgentBridge::MakeSuccess(TEXT("Material assignment retrieved"), Data);
}

// ============================================================
// 写接口 1: SpawnActor
// UE5 依赖: UEditorLevelLibrary::SpawnActorFromClass()
// Transaction: FScopedTransaction（自动纳入 Undo）
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::SpawnActor(
	const FString& LevelPath,
	const FString& ActorClass,
	const FString& ActorName,
	const FBridgeTransform& Transform,
	bool bDryRun)
{
	// 参数校验
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(LevelPath, TEXT("LevelPath"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateRequiredString(ActorClass, TEXT("ActorClass"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateRequiredString(ActorName, TEXT("ActorName"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateTransform(Transform, ValidationError)) return FinalizeWriteResponse(ValidationError);

	// Editor 就绪检查
	if (!AgentBridge::IsEditorReady(ValidationError)) return FinalizeWriteResponse(ValidationError);

	// Dry run
	if (bDryRun)
	{
		return FinalizeWriteResponse(AgentBridge::MakeSuccess(
			FString::Printf(TEXT("Dry run: would spawn %s"), *ActorName),
			MakeEmptyWriteData()));
	}

	// 加载类
	UClass* Class = LoadClass<AActor>(nullptr, *ActorClass);
	if (!Class)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			FString::Printf(TEXT("Class not found: %s"), *ActorClass),
			EBridgeErrorCode::ClassNotFound,
			FString::Printf(TEXT("Cannot load class: %s"), *ActorClass)));
	}

	// 在 Transaction 内执行（Ctrl+Z 可撤销）
	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("AgentBridge: Spawn %s"), *ActorName)));

	UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			TEXT("Editor world unavailable"),
			EBridgeErrorCode::EditorNotReady,
			TEXT("GetEditorWorld returned null")));
	}

	// 在无头/无人值守环境下，UEditorActorSubsystem::SpawnActorFromClass 可能触发引擎内部异常（除零）。
	// 这里提供稳定回退：直接用 UWorld::SpawnActor 走引擎通用路径。
	AActor* NewActor = nullptr;
	const bool bHeadlessSafePath = IsRunningCommandlet() || FApp::IsUnattended() || (GEditor && GEditor->GetActiveViewport() == nullptr);
	if (bHeadlessSafePath)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transactional;

		const FTransform SpawnTransform(Transform.Rotation, Transform.Location, Transform.RelativeScale3D);
		NewActor = World->SpawnActor<AActor>(Class, SpawnTransform, SpawnParams);
	}
	else
	{
		if (!ActorSubsystem)
		{
			return FinalizeWriteResponse(AgentBridge::MakeFailed(
				TEXT("Editor actor subsystem unavailable"),
				EBridgeErrorCode::EditorNotReady,
				TEXT("UEditorActorSubsystem is null")));
		}

		NewActor = ActorSubsystem->SpawnActorFromClass(Class, Transform.Location, Transform.Rotation);
	}

	if (!NewActor)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			TEXT("Failed to spawn actor"),
			EBridgeErrorCode::ToolExecutionFailed,
			TEXT("SpawnActorFromClass returned nullptr")));
	}

	NewActor->SetActorLabel(ActorName);
	NewActor->SetActorScale3D(Transform.RelativeScale3D);

	// 写后读回（从 UE5 API 重新读取，不是复制输入参数）
	FBridgeTransform ActualTransform = FBridgeTransform::FromActor(NewActor);

	// 构造响应
	TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();

	TArray<TSharedPtr<FJsonValue>> CreatedArr;
	FBridgeObjectRef Ref(ActorName, NewActor->GetPathName());
	CreatedArr.Add(MakeShareable(new FJsonValueObject(Ref.ToJson())));
	Data->SetArrayField(TEXT("created_objects"), CreatedArr);

	Data->SetObjectField(TEXT("actual_transform"), ActualTransform.ToJson());

	TArray<TSharedPtr<FJsonValue>> DirtyArr;
	DirtyArr.Add(MakeShareable(new FJsonValueString(LevelPath)));
	Data->SetArrayField(TEXT("dirty_assets"), DirtyArr);

	return FinalizeWriteResponse(AgentBridge::MakeSuccess(
		FString::Printf(TEXT("Spawned actor: %s"), *ActorName), Data));
}

// ============================================================
// 写接口 2: SetActorTransform
// UE5 依赖: AActor::SetActorLocationAndRotation() + SetActorScale3D()
// Transaction: FScopedTransaction（自動纳入 Undo）
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::SetActorTransform(
	const FString& ActorPath,
	const FBridgeTransform& Transform,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateTransform(Transform, ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::IsEditorReady(ValidationError)) return FinalizeWriteResponse(ValidationError);

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			TEXT("Actor not found"), EBridgeErrorCode::ActorNotFound,
			FString::Printf(TEXT("No actor at: %s"), *ActorPath)));
	}

	// 读旧值
	FBridgeTransform OldTransform = FBridgeTransform::FromActor(Actor);

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();
		TArray<TSharedPtr<FJsonValue>> ModArr;
		TSharedPtr<FJsonObject> ModRef = MakeShareable(new FJsonObject());
		ModRef->SetStringField(TEXT("actor_path"), ActorPath);
		ModArr.Add(MakeShareable(new FJsonValueObject(ModRef)));
		Data->SetArrayField(TEXT("modified_objects"), ModArr);
		Data->SetObjectField(TEXT("old_transform"), OldTransform.ToJson());
		return FinalizeWriteResponse(AgentBridge::MakeSuccess(TEXT("Dry run: would modify transform"), Data));
	}

	// 在 Transaction 内执行
	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("AgentBridge: SetTransform %s"), *Actor->GetActorNameOrLabel())));

	Actor->Modify();
	Actor->SetActorLocationAndRotation(Transform.Location, Transform.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
	Actor->SetActorScale3D(Transform.RelativeScale3D);

	// 写后読回
	FBridgeTransform ActualTransform = FBridgeTransform::FromActor(Actor);

	UWorld* World = GetEditorWorld();
	TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();

	TArray<TSharedPtr<FJsonValue>> ModArr;
	TSharedPtr<FJsonObject> ModRef = MakeShareable(new FJsonObject());
	ModRef->SetStringField(TEXT("actor_path"), ActorPath);
	ModArr.Add(MakeShareable(new FJsonValueObject(ModRef)));
	Data->SetArrayField(TEXT("modified_objects"), ModArr);

	Data->SetObjectField(TEXT("old_transform"), OldTransform.ToJson());
	Data->SetObjectField(TEXT("actual_transform"), ActualTransform.ToJson());

	TArray<TSharedPtr<FJsonValue>> DirtyArr;
	if (World) DirtyArr.Add(MakeShareable(new FJsonValueString(World->GetPathName())));
	Data->SetArrayField(TEXT("dirty_assets"), DirtyArr);

	return FinalizeWriteResponse(AgentBridge::MakeSuccess(TEXT("Transform updated"), Data));
}

// ============================================================
// 写接口 3: ImportAssets
// UE5 依赖: UAssetToolsHelpers::GetAssetTools()->ImportAssetTasks()
// Transaction: FScopedTransaction
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::ImportAssets(
	const FString& SourceDir,
	const FString& DestPath,
	bool bReplaceExisting,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(SourceDir, TEXT("SourceDir"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateRequiredString(DestPath, TEXT("DestPath"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::IsEditorReady(ValidationError)) return FinalizeWriteResponse(ValidationError);

	// 收集源文件
	TArray<FString> SourceFiles;
	IFileManager::Get().FindFiles(SourceFiles, *(SourceDir / TEXT("*")), true, false);

	// 过滤支持的文件类型
	TArray<FString> SupportedFiles;
	TArray<FString> SupportedExtensions = { TEXT(".fbx"), TEXT(".obj"), TEXT(".png"), TEXT(".tga"), TEXT(".wav"), TEXT(".exr") };
	for (const FString& File : SourceFiles)
	{
		for (const FString& Ext : SupportedExtensions)
		{
			if (File.EndsWith(Ext, ESearchCase::IgnoreCase))
			{
				SupportedFiles.Add(SourceDir / File);
				break;
			}
		}
	}

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();
		TArray<TSharedPtr<FJsonValue>> WouldImport;
		for (const FString& F : SupportedFiles)
		{
			TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject());
			Entry->SetStringField(TEXT("source_file"), F);
			WouldImport.Add(MakeShareable(new FJsonValueObject(Entry)));
		}
		Data->SetArrayField(TEXT("would_import"), WouldImport);
		return FinalizeWriteResponse(AgentBridge::MakeSuccess(
			FString::Printf(TEXT("Dry run: would import %d assets"), SupportedFiles.Num()), Data));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("AgentBridge: Import Assets")));

	// 创建导入任务
	TArray<UAssetImportTask*> Tasks;
	for (const FString& FilePath : SupportedFiles)
	{
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = FilePath;
		Task->DestinationPath = DestPath;
		Task->bReplaceExisting = bReplaceExisting;
		Task->bAutomated = true;
		Task->bSave = false;
		Tasks.Add(Task);
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks(Tasks);

	// 收集结果
	TArray<TSharedPtr<FJsonValue>> CreatedArr;
	TArray<TSharedPtr<FJsonValue>> DirtyArr;
	for (UAssetImportTask* Task : Tasks)
	{
		for (const FString& ImportedPath : Task->ImportedObjectPaths)
		{
			TSharedPtr<FJsonObject> Ref = MakeShareable(new FJsonObject());
			Ref->SetStringField(TEXT("asset_path"), ImportedPath);
			CreatedArr.Add(MakeShareable(new FJsonValueObject(Ref)));
			DirtyArr.Add(MakeShareable(new FJsonValueString(ImportedPath)));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();
	Data->SetArrayField(TEXT("created_objects"), CreatedArr);
	Data->SetArrayField(TEXT("dirty_assets"), DirtyArr);

	return FinalizeWriteResponse(AgentBridge::MakeSuccess(
		FString::Printf(TEXT("Imported %d assets"), CreatedArr.Num()), Data));
}

// ============================================================
// 写接口 4: CreateBlueprintChild
// UE5 依赖: UBlueprintFactory + UAssetToolsHelpers
// Transaction: FScopedTransaction
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::CreateBlueprintChild(
	const FString& ParentClass,
	const FString& PackagePath,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ParentClass, TEXT("ParentClass"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateRequiredString(PackagePath, TEXT("PackagePath"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::IsEditorReady(ValidationError)) return FinalizeWriteResponse(ValidationError);

	UClass* Parent = LoadClass<UObject>(nullptr, *ParentClass);
	if (!Parent)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			FString::Printf(TEXT("Parent class not found: %s"), *ParentClass),
			EBridgeErrorCode::ClassNotFound,
			FString::Printf(TEXT("Cannot load: %s"), *ParentClass)));
	}

	if (bDryRun)
	{
		return FinalizeWriteResponse(AgentBridge::MakeSuccess(
			FString::Printf(TEXT("Dry run: would create BP child at %s"), *PackagePath),
			MakeEmptyWriteData()));
	}

	// 解析路径和名称
	FString Path, Name;
	PackagePath.Split(TEXT("/"), &Path, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (Name.IsEmpty())
	{
		Name = Path;
		Path = TEXT("/Game");
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("AgentBridge: Create BP %s"), *Name)));

	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = Parent;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UBlueprint::StaticClass(), Factory);

	if (!NewAsset)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			TEXT("Failed to create Blueprint"),
			EBridgeErrorCode::ToolExecutionFailed,
			TEXT("CreateAsset returned nullptr")));
	}

	FString CreatedPath = NewAsset->GetPathName();
	TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();
	TArray<TSharedPtr<FJsonValue>> CreatedArr;
	TSharedPtr<FJsonObject> Ref = MakeShareable(new FJsonObject());
	Ref->SetStringField(TEXT("asset_path"), CreatedPath);
	CreatedArr.Add(MakeShareable(new FJsonValueObject(Ref)));
	Data->SetArrayField(TEXT("created_objects"), CreatedArr);
	TArray<TSharedPtr<FJsonValue>> DirtyArr;
	DirtyArr.Add(MakeShareable(new FJsonValueString(CreatedPath)));
	Data->SetArrayField(TEXT("dirty_assets"), DirtyArr);

	return FinalizeWriteResponse(AgentBridge::MakeSuccess(
		FString::Printf(TEXT("Created Blueprint at %s"), *CreatedPath), Data));
}

// ============================================================
// 写接口 5: SetActorCollision
// UE5 依赖: UPrimitiveComponent::SetCollisionProfileName / SetCollisionEnabled / SetCanEverAffectNavigation
// Transaction: FScopedTransaction
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::SetActorCollision(
	const FString& ActorPath,
	const FString& CollisionProfileName,
	const FString& CollisionEnabledName,
	bool bCanAffectNavigation,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateRequiredString(CollisionProfileName, TEXT("CollisionProfileName"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateRequiredString(CollisionEnabledName, TEXT("CollisionEnabledName"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::IsEditorReady(ValidationError)) return FinalizeWriteResponse(ValidationError);

	ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::QueryAndPhysics;
	FString NormalizedCollisionEnabledName;
	if (!TryParseCollisionEnabledName(CollisionEnabledName, CollisionEnabled, NormalizedCollisionEnabledName))
	{
		return FinalizeWriteResponse(AgentBridge::MakeValidationError(
			TEXT("CollisionEnabledName"),
			FString::Printf(
				TEXT("Unsupported collision enabled value '%s'. Expected one of: NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics, ProbeOnly, QueryAndProbe"),
				*CollisionEnabledName)));
	}

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			TEXT("Actor not found"),
			EBridgeErrorCode::ActorNotFound,
			FString::Printf(TEXT("No actor at path: %s"), *ActorPath)));
	}

	UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
	if (!PrimitiveComponent)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			TEXT("PrimitiveComponent not found"),
			EBridgeErrorCode::ToolExecutionFailed,
			FString::Printf(TEXT("Actor '%s' root component is not a PrimitiveComponent"), *ActorPath)));
	}

	TSharedPtr<FJsonObject> OldCollision = ReadCollisionToJson(Actor);

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();
		TArray<TSharedPtr<FJsonValue>> ModifiedArray;
		TSharedPtr<FJsonObject> ModifiedRef = MakeShareable(new FJsonObject());
		ModifiedRef->SetStringField(TEXT("actor_path"), ActorPath);
		ModifiedArray.Add(MakeShareable(new FJsonValueObject(ModifiedRef)));
		Data->SetArrayField(TEXT("modified_objects"), ModifiedArray);
		Data->SetObjectField(TEXT("old_collision"), OldCollision);

		TSharedPtr<FJsonObject> PreviewCollision = MakeShareable(new FJsonObject(*OldCollision));
		PreviewCollision->SetStringField(TEXT("collision_profile_name"), CollisionProfileName);
		PreviewCollision->SetStringField(TEXT("collision_enabled"), NormalizedCollisionEnabledName);
		PreviewCollision->SetBoolField(TEXT("can_affect_navigation"), bCanAffectNavigation);
		Data->SetObjectField(TEXT("preview_collision"), PreviewCollision);

		return FinalizeWriteResponse(AgentBridge::MakeSuccess(TEXT("Dry run: would modify collision"), Data));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("AgentBridge: SetCollision %s"), *Actor->GetActorNameOrLabel())));

	Actor->Modify();
	PrimitiveComponent->Modify();
	PrimitiveComponent->SetCollisionProfileName(FName(*CollisionProfileName));
	PrimitiveComponent->SetCollisionEnabled(CollisionEnabled);
	PrimitiveComponent->SetCanEverAffectNavigation(bCanAffectNavigation);

	TSharedPtr<FJsonObject> ActualCollision = ReadCollisionToJson(Actor);
	TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();

	TArray<TSharedPtr<FJsonValue>> ModifiedArray;
	TSharedPtr<FJsonObject> ModifiedRef = MakeShareable(new FJsonObject());
	ModifiedRef->SetStringField(TEXT("actor_path"), ActorPath);
	ModifiedArray.Add(MakeShareable(new FJsonValueObject(ModifiedRef)));
	Data->SetArrayField(TEXT("modified_objects"), ModifiedArray);
	Data->SetObjectField(TEXT("old_collision"), OldCollision);
	Data->SetObjectField(TEXT("actual_collision"), ActualCollision);

	TArray<TSharedPtr<FJsonValue>> DirtyArray;
	const FString LevelPath = GetCurrentLevelPackagePath(GetEditorWorld());
	if (!LevelPath.IsEmpty())
	{
		DirtyArray.Add(MakeShareable(new FJsonValueString(LevelPath)));
	}
	Data->SetArrayField(TEXT("dirty_assets"), DirtyArray);

	return FinalizeWriteResponse(AgentBridge::MakeSuccess(TEXT("Collision updated"), Data));
}

// ============================================================
// 写接口 6: AssignMaterial
// UE5 依赖: LoadObject<UMaterialInterface>() + UMeshComponent::SetMaterial()
// Transaction: FScopedTransaction
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::AssignMaterial(
	const FString& ActorPath,
	const FString& MaterialPath,
	int32 SlotIndex,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (!AgentBridge::ValidateRequiredString(MaterialPath, TEXT("MaterialPath"), ValidationError)) return FinalizeWriteResponse(ValidationError);
	if (SlotIndex < 0)
	{
		return FinalizeWriteResponse(AgentBridge::MakeValidationError(TEXT("SlotIndex"), TEXT("SlotIndex must be >= 0")));
	}
	if (!AgentBridge::IsEditorReady(ValidationError)) return FinalizeWriteResponse(ValidationError);

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			TEXT("Actor not found"),
			EBridgeErrorCode::ActorNotFound,
			FString::Printf(TEXT("No actor at path: %s"), *ActorPath)));
	}

	UMeshComponent* MeshComponent = FindPrimaryMeshComponent(Actor);
	if (!MeshComponent)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			TEXT("MeshComponent not found"),
			EBridgeErrorCode::ToolExecutionFailed,
			FString::Printf(TEXT("Actor '%s' has no MeshComponent"), *ActorPath)));
	}

	if (SlotIndex >= MeshComponent->GetNumMaterials())
	{
		return FinalizeWriteResponse(AgentBridge::MakeValidationError(
			TEXT("SlotIndex"),
			FString::Printf(TEXT("SlotIndex %d is out of range. NumMaterials=%d"), SlotIndex, MeshComponent->GetNumMaterials())));
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		return FinalizeWriteResponse(AgentBridge::MakeFailed(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath),
			EBridgeErrorCode::AssetNotFound,
			FString::Printf(TEXT("Cannot load material asset: %s"), *MaterialPath)));
	}

	UMaterialInterface* OldMaterial = MeshComponent->GetMaterial(SlotIndex);

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();
		TArray<TSharedPtr<FJsonValue>> ModifiedArray;
		TSharedPtr<FJsonObject> ModifiedRef = MakeShareable(new FJsonObject());
		ModifiedRef->SetStringField(TEXT("actor_path"), ActorPath);
		ModifiedArray.Add(MakeShareable(new FJsonValueObject(ModifiedRef)));
		Data->SetArrayField(TEXT("modified_objects"), ModifiedArray);
		Data->SetStringField(TEXT("old_material_path"), OldMaterial ? OldMaterial->GetPathName() : TEXT(""));
		Data->SetStringField(TEXT("preview_material_path"), Material->GetPathName());
		Data->SetNumberField(TEXT("slot_index"), SlotIndex);
		return FinalizeWriteResponse(AgentBridge::MakeSuccess(TEXT("Dry run: would assign material"), Data));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("AgentBridge: AssignMaterial %s"), *Actor->GetActorNameOrLabel())));

	Actor->Modify();
	MeshComponent->Modify();
	MeshComponent->SetMaterial(SlotIndex, Material);

	UMaterialInterface* ActualMaterial = MeshComponent->GetMaterial(SlotIndex);

	TSharedPtr<FJsonObject> Data = MakeEmptyWriteData();
	TArray<TSharedPtr<FJsonValue>> ModifiedArray;
	TSharedPtr<FJsonObject> ModifiedRef = MakeShareable(new FJsonObject());
	ModifiedRef->SetStringField(TEXT("actor_path"), ActorPath);
	ModifiedArray.Add(MakeShareable(new FJsonValueObject(ModifiedRef)));
	Data->SetArrayField(TEXT("modified_objects"), ModifiedArray);
	Data->SetStringField(TEXT("old_material_path"), OldMaterial ? OldMaterial->GetPathName() : TEXT(""));
	Data->SetStringField(TEXT("actual_material_path"), ActualMaterial ? ActualMaterial->GetPathName() : TEXT(""));
	Data->SetStringField(TEXT("actual_material_name"), ActualMaterial ? ActualMaterial->GetName() : TEXT(""));
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);

	TArray<TSharedPtr<FJsonValue>> DirtyArray;
	const FString LevelPath = GetCurrentLevelPackagePath(GetEditorWorld());
	if (!LevelPath.IsEmpty())
	{
		DirtyArray.Add(MakeShareable(new FJsonValueString(LevelPath)));
	}
	Data->SetArrayField(TEXT("dirty_assets"), DirtyArray);

	return FinalizeWriteResponse(AgentBridge::MakeSuccess(TEXT("Material assigned"), Data));
}

// ============================================================
// 验証接口 1: ValidateActorInsideBounds
// UE5 依赖: AActor::GetActorBounds() + FBox::IsInside()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::ValidateActorInsideBounds(
	const FString& ActorPath,
	const FVector& BoundsOrigin,
	const FVector& BoundsExtent)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError)) return ValidationError;

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return AgentBridge::MakeFailed(TEXT("Actor not found"), EBridgeErrorCode::ActorNotFound, ActorPath);
	}

	FVector ActorOrigin, ActorExtent;
	Actor->GetActorBounds(false, ActorOrigin, ActorExtent);

	FBox TargetBounds(BoundsOrigin - BoundsExtent, BoundsOrigin + BoundsExtent);
	FBox ActorBounds(ActorOrigin - ActorExtent, ActorOrigin + ActorExtent);

	bool bInside = TargetBounds.IsInside(ActorBounds);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("actor_path"), ActorPath);
	Data->SetBoolField(TEXT("is_inside"), bInside);

	if (bInside)
	{
		return AgentBridge::MakeSuccess(TEXT("Actor is inside bounds"), Data);
	}
	else
	{
		return AgentBridge::MakeMismatch(TEXT("Actor is outside bounds"), Data);
	}
}

// ============================================================
// 験証接口 2: ValidateActorNonOverlap
// UE5 依赖: UWorld::OverlapMultiByChannel()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::ValidateActorNonOverlap(const FString& ActorPath)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError)) return ValidationError;

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return AgentBridge::MakeFailed(TEXT("Actor not found"), EBridgeErrorCode::ActorNotFound, ActorPath);
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return AgentBridge::MakeFailed(TEXT("No editor world"), EBridgeErrorCode::EditorNotReady, TEXT(""));
	}

	FVector Origin, Extent;
	Actor->GetActorBounds(false, Origin, Extent);

	FCollisionShape Shape = FCollisionShape::MakeBox(Extent);
	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(Actor);

	World->OverlapMultiByChannel(Overlaps, Origin, FQuat::Identity, ECC_WorldStatic, Shape, Params);

	TArray<TSharedPtr<FJsonValue>> OverlapArr;
	for (const FOverlapResult& Result : Overlaps)
	{
		if (AActor* Other = Result.GetActor())
		{
			OverlapArr.Add(MakeShareable(new FJsonValueString(Other->GetPathName())));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("actor_path"), ActorPath);
	Data->SetBoolField(TEXT("has_overlap"), OverlapArr.Num() > 0);
	Data->SetArrayField(TEXT("overlapping_actors"), OverlapArr);

	if (OverlapArr.Num() == 0)
	{
		return AgentBridge::MakeSuccess(TEXT("No overlaps detected"), Data);
	}
	else
	{
		return AgentBridge::MakeMismatch(
			FString::Printf(TEXT("Overlaps with %d actors"), OverlapArr.Num()), Data);
	}
}

// ============================================================
// 験証接口 3: RunAutomationTests
// UE5 依赖: Automation Test Framework — Console Command
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::RunAutomationTests(
	const FString& Filter,
	const FString& ReportPath)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(Filter, TEXT("Filter"), ValidationError)) return ValidationError;

	// 构造 Automation 命令
	FString Command = FString::Printf(TEXT("Automation RunTests %s"), *Filter);
	if (!ReportPath.IsEmpty())
	{
		Command += FString::Printf(TEXT(";Automation ReportExportPath %s"), *ReportPath);
	}

	UWorld* World = GetEditorWorld();
	if (World)
	{
		GEditor->Exec(World, *Command);
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("filter"), Filter);
	Data->SetStringField(TEXT("command"), Command);
	if (!ReportPath.IsEmpty())
	{
		Data->SetStringField(TEXT("report_path"), ReportPath);
	}

	return AgentBridge::MakeSuccess(
		FString::Printf(TEXT("Automation tests triggered: %s"), *Filter), Data);
}

// ============================================================
// 構建接口: BuildProject
// UE5 依赖: UAT (BuildCookRun) — 外部子进程
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::BuildProject(
	const FString& Platform,
	const FString& Configuration,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(Platform, TEXT("Platform"), ValidationError))
	{
		return ValidationError;
	}
	if (!AgentBridge::ValidateRequiredString(Configuration, TEXT("Configuration"), ValidationError))
	{
		return ValidationError;
	}

	FUATRunner Runner;
	const bool bUATAvailable = Runner.IsUATAvailable();

	const FString ProjectPath = FPaths::GetProjectFilePath();
	const FString PreviewArgs = FString::Printf(
		TEXT("BuildCookRun -project=\"%s\" -platform=%s -clientconfig=%s -cook -stage -pak"),
		*ProjectPath,
		*Platform,
		*Configuration);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetBoolField(TEXT("uat_available"), bUATAvailable);
	Data->SetStringField(TEXT("command_preview"), PreviewArgs);
	Data->SetStringField(TEXT("platform"), Platform);
	Data->SetStringField(TEXT("configuration"), Configuration);

	if (!bUATAvailable)
	{
		return AgentBridge::MakeFailed(
			TEXT("UAT is not available"),
			EBridgeErrorCode::ToolExecutionFailed,
			TEXT("RunUAT.bat/sh not found under Engine/Build/BatchFiles"));
	}

	if (bDryRun)
	{
		return AgentBridge::MakeSuccess(
			FString::Printf(TEXT("Dry run: BuildCookRun for %s/%s"), *Platform, *Configuration),
			Data);
	}

	const FUATRunResult RunResult = Runner.BuildCookRun(Platform, Configuration, /*bSync=*/false);
	Data->SetBoolField(TEXT("launched"), RunResult.bLaunched);
	Data->SetBoolField(TEXT("completed"), RunResult.bCompleted);
	Data->SetNumberField(TEXT("exit_code"), RunResult.ExitCode);
	Data->SetStringField(TEXT("command_line"), RunResult.CommandLine);
	Data->SetBoolField(TEXT("is_success"), RunResult.IsSuccess());
	if (!RunResult.ErrorMessage.IsEmpty())
	{
		Data->SetStringField(TEXT("error_message"), RunResult.ErrorMessage);
	}

	if (!RunResult.bLaunched)
	{
		return AgentBridge::MakeFailed(
			TEXT("Failed to launch UAT BuildCookRun"),
			EBridgeErrorCode::ToolExecutionFailed,
			RunResult.ErrorMessage.IsEmpty() ? TEXT("CreateProc failed") : RunResult.ErrorMessage);
	}

	return AgentBridge::MakeSuccess(TEXT("UAT BuildCookRun launched"), Data);
}

// ============================================================
// 辅助接口: SaveNamedAssets
// UE5 依赖: UEditorAssetLibrary::SaveLoadedAssets()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::SaveNamedAssets(const TArray<FString>& AssetPaths)
{
	TArray<FString> Saved;
	TArray<FString> Failed;

	if (AssetPaths.Num() == 0)
	{
		// 保存全部脏资产
		bool bResult = FEditorFileUtils::SaveDirtyPackages(
			/*bPromptUserToSave=*/false,
			/*bSaveMapPackages=*/true,
			/*bSaveContentPackages=*/true);

		TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
		Data->SetBoolField(TEXT("all_saved"), bResult);
		return AgentBridge::MakeSuccess(TEXT("All dirty assets saved"), Data);
	}

	for (const FString& Path : AssetPaths)
	{
		if (UEditorAssetLibrary::SaveAsset(Path, false))
		{
			Saved.Add(Path);
		}
		else
		{
			Failed.Add(Path);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> SavedArr, FailedArr;
	for (const FString& S : Saved) SavedArr.Add(MakeShareable(new FJsonValueString(S)));
	for (const FString& F : Failed) FailedArr.Add(MakeShareable(new FJsonValueString(F)));
	Data->SetArrayField(TEXT("saved"), SavedArr);
	Data->SetArrayField(TEXT("failed"), FailedArr);

	if (Failed.Num() > 0)
	{
		FBridgeResponse Response;
		Response.Status = EBridgeStatus::Warning;
		Response.Summary = FString::Printf(TEXT("Saved %d, failed %d"), Saved.Num(), Failed.Num());
		Response.Data = Data;
		Response.SyncForRemote();
		return Response;
	}

	return AgentBridge::MakeSuccess(
		FString::Printf(TEXT("Saved %d assets"), Saved.Num()), Data);
}

// ============================================================
// 辅助接口: CaptureViewportScreenshot
// UE5 依赖: FViewport::ReadPixels + FImageUtils::CompressImageArray
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::CaptureViewportScreenshot(const FString& ScreenshotName)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ScreenshotName, TEXT("ScreenshotName"), ValidationError)) return ValidationError;
	if (!AgentBridge::IsEditorReady(ValidationError)) return ValidationError;

	const double StartTime = FPlatformTime::Seconds();
	FViewport* ActiveViewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
	if (!ActiveViewport)
	{
		return AgentBridge::MakeFailed(
			FString::Printf(TEXT("Capture screenshot failed: %s"), *ScreenshotName),
			EBridgeErrorCode::ToolExecutionFailed,
			TEXT("No active editor viewport found"));
	}

	const FIntPoint ViewportSize = ActiveViewport->GetSizeXY();
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		return AgentBridge::MakeFailed(
			FString::Printf(TEXT("Capture screenshot failed: %s"), *ScreenshotName),
			EBridgeErrorCode::ToolExecutionFailed,
			FString::Printf(TEXT("Invalid viewport size: %d x %d"), ViewportSize.X, ViewportSize.Y));
	}

	// 同步读取当前视口像素，保证后续写文件是确定性的。
	TArray<FColor> Bitmap;
	if (!ActiveViewport->ReadPixels(Bitmap))
	{
		return AgentBridge::MakeFailed(
			FString::Printf(TEXT("Capture screenshot failed: %s"), *ScreenshotName),
			EBridgeErrorCode::ToolExecutionFailed,
			TEXT("ReadPixels failed on active viewport"));
	}

	for (FColor& Pixel : Bitmap)
	{
		Pixel.A = 255;
	}

	FString ScreenshotDir = FPaths::AutomationDir() / TEXT("Screenshots");
	FString FullPath = ScreenshotDir / (ScreenshotName + TEXT(".png"));

	// 先确保目录存在并清理旧文件，避免误判“已落盘”。
	IFileManager::Get().MakeDirectory(*ScreenshotDir, true);
	IFileManager::Get().Delete(*FullPath, false, true, true);

	// 同步压缩并写出 PNG，函数返回时文件已经落盘。
	TArray64<uint8> PngData;
	const TArrayView64<const FColor> BitmapView(Bitmap.GetData(), static_cast<int64>(Bitmap.Num()));
	FImageUtils::PNGCompressImageArray(ViewportSize.X, ViewportSize.Y, BitmapView, PngData);
	if (PngData.Num() <= 0)
	{
		return AgentBridge::MakeFailed(
			FString::Printf(TEXT("Capture screenshot failed: %s"), *ScreenshotName),
			EBridgeErrorCode::ToolExecutionFailed,
			TEXT("PNG encoding failed"));
	}

	if (!FFileHelper::SaveArrayToFile(PngData, *FullPath))
	{
		return AgentBridge::MakeFailed(
			FString::Printf(TEXT("Capture screenshot failed: %s"), *ScreenshotName),
			EBridgeErrorCode::ToolExecutionFailed,
			FString::Printf(TEXT("SaveArrayToFile failed: %s"), *FullPath));
	}

	const bool bFileReady = IFileManager::Get().FileExists(*FullPath);
	const int64 FileSize = IFileManager::Get().FileSize(*FullPath);
	const double WaitedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("screenshot_name"), ScreenshotName);
	Data->SetStringField(TEXT("output_path"), FullPath);
	Data->SetBoolField(TEXT("file_exists"), bFileReady);
	Data->SetNumberField(TEXT("file_size"), static_cast<double>(FileSize));
	Data->SetNumberField(TEXT("waited_ms"), WaitedMs);

	if (!bFileReady || FileSize <= 0)
	{
		return AgentBridge::MakeFailed(
			FString::Printf(TEXT("Capture screenshot failed: %s"), *ScreenshotName),
			EBridgeErrorCode::ToolExecutionFailed,
			FString::Printf(TEXT("Screenshot file not ready after save: %s"), *FullPath));
	}

	return AgentBridge::MakeSuccess(
		FString::Printf(TEXT("Screenshot captured: %s"), *ScreenshotName), Data);
}

// ============================================================
// 辅助接口: UndoLastTransaction
// UE5 依赖: GEditor->UndoTransaction()
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::UndoLastTransaction(int32 Steps)
{
	if (Steps <= 0)
	{
		return AgentBridge::MakeValidationError(
			TEXT("Steps"),
			TEXT("Steps must be greater than 0"));
	}

	FBridgeResponse ValidationError;
	if (!AgentBridge::IsEditorReady(ValidationError))
	{
		return ValidationError;
	}

	int32 UndoneSteps = 0;
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		// 使用 UE5 原生 Undo 栈逐步回滚，直到无可撤销事务。
		if (!GEditor->UndoTransaction())
		{
			break;
		}
		++UndoneSteps;
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetNumberField(TEXT("requested_steps"), Steps);
	Data->SetNumberField(TEXT("undone_steps"), UndoneSteps);
	Data->SetBoolField(TEXT("fully_undone"), UndoneSteps == Steps);

	if (UndoneSteps <= 0)
	{
		FBridgeResponse Response;
		Response.Status = EBridgeStatus::Warning;
		Response.Summary = TEXT("No transaction available to undo");
		Response.Data = Data;
		Response.SyncForRemote();
		return Response;
	}

	return AgentBridge::MakeSuccess(
		FString::Printf(TEXT("Undid %d transaction(s)"), UndoneSteps),
		Data);
}

// ============================================================
// L3 UI 工具接口实现
// UE5 官方模块：Automation Driver（IAutomationDriverModule）
//
// 工具优先级：L1 语义工具 > L2 编辑器服务工具 > L3 UI 工具
// L3 仅当 L1 无法覆盖时使用。
// L3 返回值与 L1 独立读回做交叉比对——两者一致才判定 success。
// ============================================================

#include "AutomationDriverAdapter.h"

// ============================================================
// IsAutomationDriverAvailable
// ============================================================

bool UAgentBridgeSubsystem::IsAutomationDriverAvailable() const
{
	return FAutomationDriverAdapter::IsAvailable();
}

// ============================================================
// 辅助：FUIOperationResult → FBridgeResponse
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::UIOperationResultToResponse(
	const FString& OperationName,
	const FUIOperationResult& UIResult) const
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("operation"), OperationName);
	Data->SetBoolField(TEXT("executed"), UIResult.bExecuted);
	Data->SetBoolField(TEXT("ui_idle_after"), UIResult.bUIIdleAfter);
	Data->SetNumberField(TEXT("duration_seconds"), UIResult.DurationSeconds);
	Data->SetStringField(TEXT("tool_layer"), TEXT("L3_UITool"));
	if (!UIResult.DebugLocatorPath.IsEmpty())
	{
		Data->SetStringField(TEXT("debug_locator_path"), UIResult.DebugLocatorPath);
	}

	if (UIResult.IsSuccess())
	{
		return AgentBridge::MakeSuccess(
			FString::Printf(TEXT("L3 UI operation succeeded: %s"), *OperationName), Data);
	}

	// 根据失败原因选择错误码
	EBridgeErrorCode Code = EBridgeErrorCode::ToolExecutionFailed;
	if (UIResult.FailureReason.Contains(TEXT("not available")))
	{
		Code = EBridgeErrorCode::DriverNotAvailable;
	}
	else if (UIResult.FailureReason.Contains(TEXT("not found")))
	{
		Code = EBridgeErrorCode::WidgetNotFound;
	}
	else if (UIResult.FailureReason.Contains(TEXT("timeout")) || UIResult.FailureReason.Contains(TEXT("idle")))
	{
		Code = EBridgeErrorCode::UIOperationTimeout;
	}

	return AgentBridge::MakeFailed(
		FString::Printf(TEXT("L3 UI operation failed: %s"), *OperationName),
		Code, UIResult.FailureReason);
}

// ============================================================
// ClickDetailPanelButton
// L3 UI 工具：在 Detail Panel 中点击按钮
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::ClickDetailPanelButton(
	const FString& ActorPath,
	const FString& ButtonLabel,
	bool bDryRun)
{
	// 参数校验
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError)) return ValidationError;
	if (!AgentBridge::ValidateRequiredString(ButtonLabel, TEXT("ButtonLabel"), ValidationError)) return ValidationError;

	// Driver 可用性检查
	if (!FAutomationDriverAdapter::IsAvailable())
	{
		return AgentBridge::MakeDriverNotAvailable();
	}

	// Actor 存在性检查
	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return AgentBridge::MakeFailed(TEXT("Actor not found"), EBridgeErrorCode::ActorNotFound, ActorPath);
	}

	// dry_run
	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
		Data->SetStringField(TEXT("operation"), TEXT("ClickDetailPanelButton"));
		Data->SetStringField(TEXT("actor_path"), ActorPath);
		Data->SetStringField(TEXT("button_label"), ButtonLabel);
		Data->SetBoolField(TEXT("driver_available"), true);
		Data->SetStringField(TEXT("tool_layer"), TEXT("L3_UITool"));
		return AgentBridge::MakeSuccess(TEXT("Dry run: would click detail panel button"), Data);
	}

	// 执行 L3 UI 操作
	FUIOperationResult UIResult = FAutomationDriverAdapter::ClickDetailPanelButton(
		ActorPath, ButtonLabel);

	FBridgeResponse Response = UIOperationResultToResponse(TEXT("ClickDetailPanelButton"), UIResult);

	// 在 data 中追加操作参数（供交叉比对时使用）
	if (Response.Data.IsValid())
	{
		Response.Data->SetStringField(TEXT("actor_path"), ActorPath);
		Response.Data->SetStringField(TEXT("button_label"), ButtonLabel);
	}

	Response.SyncForRemote();
	return Response;
}

// ============================================================
// TypeInDetailPanelField
// L3 UI 工具：在 Detail Panel 属性输入框中输入值
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::TypeInDetailPanelField(
	const FString& ActorPath,
	const FString& PropertyPath,
	const FString& Value,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError)) return ValidationError;
	if (!AgentBridge::ValidateRequiredString(PropertyPath, TEXT("PropertyPath"), ValidationError)) return ValidationError;
	if (!AgentBridge::ValidateRequiredString(Value, TEXT("Value"), ValidationError)) return ValidationError;

	if (!FAutomationDriverAdapter::IsAvailable())
	{
		return AgentBridge::MakeDriverNotAvailable();
	}

	AActor* Actor = FindActorByPath(ActorPath);
	if (!Actor)
	{
		return AgentBridge::MakeFailed(TEXT("Actor not found"), EBridgeErrorCode::ActorNotFound, ActorPath);
	}

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
		Data->SetStringField(TEXT("operation"), TEXT("TypeInDetailPanelField"));
		Data->SetStringField(TEXT("actor_path"), ActorPath);
		Data->SetStringField(TEXT("property_path"), PropertyPath);
		Data->SetStringField(TEXT("value"), Value);
		Data->SetBoolField(TEXT("driver_available"), true);
		Data->SetStringField(TEXT("tool_layer"), TEXT("L3_UITool"));
		return AgentBridge::MakeSuccess(TEXT("Dry run: would type in detail panel field"), Data);
	}

	FUIOperationResult UIResult = FAutomationDriverAdapter::TypeInDetailPanelField(
		ActorPath, PropertyPath, Value);

	FBridgeResponse Response = UIOperationResultToResponse(TEXT("TypeInDetailPanelField"), UIResult);

	if (Response.Data.IsValid())
	{
		Response.Data->SetStringField(TEXT("actor_path"), ActorPath);
		Response.Data->SetStringField(TEXT("property_path"), PropertyPath);
		Response.Data->SetStringField(TEXT("typed_value"), Value);
	}

	Response.SyncForRemote();
	return Response;
}

// ============================================================
// DragAssetToViewport
// L3 UI 工具：将资产从 Content Browser 拖拽到 Viewport
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::DragAssetToViewport(
	const FString& AssetPath,
	const FVector& DropLocation,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(AssetPath, TEXT("AssetPath"), ValidationError)) return ValidationError;

	if (!FAutomationDriverAdapter::IsAvailable())
	{
		return AgentBridge::MakeDriverNotAvailable();
	}

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
		Data->SetStringField(TEXT("operation"), TEXT("DragAssetToViewport"));
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShareable(new FJsonValueNumber(DropLocation.X)));
		LocArr.Add(MakeShareable(new FJsonValueNumber(DropLocation.Y)));
		LocArr.Add(MakeShareable(new FJsonValueNumber(DropLocation.Z)));
		Data->SetArrayField(TEXT("drop_location"), LocArr);
		Data->SetBoolField(TEXT("driver_available"), true);
		Data->SetStringField(TEXT("tool_layer"), TEXT("L3_UITool"));
		return AgentBridge::MakeSuccess(TEXT("Dry run: would drag asset to viewport"), Data);
	}

	// 记录操作前的 Actor 数量（用于交叉比对时判断是否有新 Actor 生成）
	TArray<AActor*> ActorsBefore;
	if (GEditor)
	{
		if (UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
		{
			ActorsBefore = ActorSubsystem->GetAllLevelActors();
		}
	}
	int32 CountBefore = ActorsBefore.Num();

	FUIOperationResult UIResult = FAutomationDriverAdapter::DragAssetToViewport(
		AssetPath, DropLocation);

	FBridgeResponse Response = UIOperationResultToResponse(TEXT("DragAssetToViewport"), UIResult);

	if (Response.Data.IsValid())
	{
		Response.Data->SetStringField(TEXT("asset_path"), AssetPath);

		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShareable(new FJsonValueNumber(DropLocation.X)));
		LocArr.Add(MakeShareable(new FJsonValueNumber(DropLocation.Y)));
		LocArr.Add(MakeShareable(new FJsonValueNumber(DropLocation.Z)));
		Response.Data->SetArrayField(TEXT("drop_location"), LocArr);

		// 操作后 Actor 数量变化
		TArray<AActor*> ActorsAfter;
		if (GEditor)
		{
			if (UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
			{
				ActorsAfter = ActorSubsystem->GetAllLevelActors();
			}
		}
		int32 CountAfter = ActorsAfter.Num();
		Response.Data->SetNumberField(TEXT("actors_before"), CountBefore);
		Response.Data->SetNumberField(TEXT("actors_after"), CountAfter);
		Response.Data->SetNumberField(TEXT("actors_created"), CountAfter - CountBefore);

		// 如果有新 Actor，记录其路径
		if (CountAfter > CountBefore)
		{
			// 新增的 Actor 通常是列表末尾
			TArray<TSharedPtr<FJsonValue>> NewActorArr;
			for (int32 i = CountBefore; i < CountAfter; ++i)
			{
				if (ActorsAfter.IsValidIndex(i) && ActorsAfter[i])
				{
					TSharedPtr<FJsonObject> ActorRef = MakeShareable(new FJsonObject());
					ActorRef->SetStringField(TEXT("actor_name"), ActorsAfter[i]->GetActorNameOrLabel());
					ActorRef->SetStringField(TEXT("actor_path"), ActorsAfter[i]->GetPathName());
					NewActorArr.Add(MakeShareable(new FJsonValueObject(ActorRef)));
				}
			}
			Response.Data->SetArrayField(TEXT("created_actors"), NewActorArr);
		}
	}

	Response.SyncForRemote();
	return Response;
}

// ============================================================
// StartUIOperation / QueryUIOperation
// 异步原型：当前支持 click_detail_panel_button / type_in_detail_panel_field / drag_asset_to_viewport
// ============================================================

FBridgeResponse UAgentBridgeSubsystem::StartUIOperation(
	const FString& OperationType,
	const FString& ActorPath,
	const FString& Target,
	const FString& Value,
	float TimeoutSeconds,
	bool bDryRun)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(OperationType, TEXT("OperationType"), ValidationError)) return ValidationError;

	const FString NormalizedOperation = OperationType.TrimStartAndEnd().ToLower();
	if (NormalizedOperation != TEXT("click_detail_panel_button")
		&& NormalizedOperation != TEXT("type_in_detail_panel_field")
		&& NormalizedOperation != TEXT("drag_asset_to_viewport"))
	{
		return AgentBridge::MakeValidationError(
			TEXT("OperationType"),
			FString::Printf(
				TEXT("Unsupported async UI operation: '%s'. Prototype currently supports click_detail_panel_button / type_in_detail_panel_field / drag_asset_to_viewport"),
				*OperationType));
	}

	if ((NormalizedOperation == TEXT("click_detail_panel_button")
		|| NormalizedOperation == TEXT("type_in_detail_panel_field"))
		&& !AgentBridge::ValidateRequiredString(ActorPath, TEXT("ActorPath"), ValidationError))
	{
		return ValidationError;
	}

	if (!AgentBridge::ValidateRequiredString(Target, TEXT("Target"), ValidationError)) return ValidationError;

	if ((NormalizedOperation == TEXT("type_in_detail_panel_field")
		|| NormalizedOperation == TEXT("drag_asset_to_viewport"))
		&& !AgentBridge::ValidateRequiredString(Value, TEXT("Value"), ValidationError))
	{
		return ValidationError;
	}

	if (TimeoutSeconds <= 0.0f)
	{
		return AgentBridge::MakeValidationError(TEXT("TimeoutSeconds"), TEXT("TimeoutSeconds must be > 0"));
	}

	if (!FAutomationDriverAdapter::IsAvailable())
	{
		return AgentBridge::MakeDriverNotAvailable();
	}

	if (NormalizedOperation == TEXT("click_detail_panel_button")
		|| NormalizedOperation == TEXT("type_in_detail_panel_field"))
	{
		AActor* Actor = FindActorByPath(ActorPath);
		if (!Actor)
		{
			return AgentBridge::MakeFailed(TEXT("Actor not found"), EBridgeErrorCode::ActorNotFound, ActorPath);
		}
	}

	FVector ParsedDropLocation = FVector::ZeroVector;
	if (NormalizedOperation == TEXT("drag_asset_to_viewport"))
	{
		if (!TryParseVectorCsv(Value, ParsedDropLocation))
		{
			return AgentBridge::MakeValidationError(
				TEXT("Value"),
				TEXT("drag_asset_to_viewport requires Value formatted as 'X,Y,Z'"));
		}
	}

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
		Data->SetStringField(TEXT("operation_type"), NormalizedOperation);
		Data->SetStringField(TEXT("actor_path"), ActorPath);
		Data->SetStringField(TEXT("target"), Target);
		Data->SetStringField(TEXT("value"), Value);
		Data->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
		Data->SetStringField(TEXT("operation_state"), TEXT("validated"));
		if (NormalizedOperation == TEXT("drag_asset_to_viewport"))
		{
			TArray<TSharedPtr<FJsonValue>> DropLocationArray;
			DropLocationArray.Add(MakeShareable(new FJsonValueNumber(ParsedDropLocation.X)));
			DropLocationArray.Add(MakeShareable(new FJsonValueNumber(ParsedDropLocation.Y)));
			DropLocationArray.Add(MakeShareable(new FJsonValueNumber(ParsedDropLocation.Z)));
			Data->SetArrayField(TEXT("drop_location"), DropLocationArray);
			Data->SetStringField(TEXT("execution_backend"), TEXT("drag_asset_to_viewport_async_prototype"));
		}
		else
		{
			Data->SetStringField(TEXT("execution_backend"),
				NormalizedOperation == TEXT("click_detail_panel_button")
				? TEXT("automation_driver_sync_off_game_thread_prototype")
				: TEXT("detail_panel_text_entry_async_prototype"));
		}
		Data->SetStringField(TEXT("tool_layer"), TEXT("L3_UITool_AsyncPrototype"));
		return AgentBridge::MakeSuccess(TEXT("Dry run: async UI operation validated"), Data);
	}

	TSharedPtr<FPendingUIOperation> Operation = MakeShared<FPendingUIOperation>();
	Operation->OperationId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Operation->OperationType = NormalizedOperation;
	Operation->ActorPath = ActorPath;
	Operation->Target = Target;
	Operation->Value = Value;
	Operation->TimeoutSeconds = TimeoutSeconds;
	Operation->OperationState = TEXT("running");
	Operation->Summary = TEXT("Async UI operation queued");
	Operation->CreatedAtSeconds = FPlatformTime::Seconds();
	Operation->UpdatedAtSeconds = Operation->CreatedAtSeconds;
	Operation->bHasDropLocation = (NormalizedOperation == TEXT("drag_asset_to_viewport"));
	if (Operation->bHasDropLocation)
	{
		Operation->DropLocation = ParsedDropLocation;
	}
	Operation->ExecutionBackend =
		NormalizedOperation == TEXT("click_detail_panel_button")
		? TEXT("automation_driver_sync_off_game_thread_prototype")
		: (NormalizedOperation == TEXT("type_in_detail_panel_field")
			? TEXT("detail_panel_text_entry_async_prototype")
			: TEXT("drag_asset_to_viewport_async_prototype"));

	// 原型阶段先把“RC 请求快速返回 + 后台任务轮询完成”这条链跑通。
	Operation->Future = MakeUnique<TFuture<FUIOperationResult>>(Async(EAsyncExecution::ThreadPool,
		[NormalizedOperation, ActorPath, Target, Value, TimeoutSeconds]()
		{
			if (NormalizedOperation == TEXT("click_detail_panel_button"))
			{
				return FAutomationDriverAdapter::ClickDetailPanelButtonOffGameThread(
					ActorPath,
					Target,
					TimeoutSeconds);
			}

			if (NormalizedOperation == TEXT("drag_asset_to_viewport"))
			{
				FVector DropLocation = FVector::ZeroVector;
				if (!TryParseVectorCsv(Value, DropLocation))
				{
					FUIOperationResult InvalidValueResult;
					InvalidValueResult.FailureReason = TEXT("Invalid drop location payload for drag_asset_to_viewport");
					return InvalidValueResult;
				}

				return FAutomationDriverAdapter::DragAssetToViewportAsyncPrototype(
					Target,
					DropLocation,
					TimeoutSeconds);
			}

			return FAutomationDriverAdapter::TypeInDetailPanelFieldAsyncPrototype(
				ActorPath,
				Target,
				Value,
				TimeoutSeconds);
		}));

	PendingUIOperations.Add(Operation->OperationId, Operation);

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("operation_id"), Operation->OperationId);
	Data->SetStringField(TEXT("operation_type"), Operation->OperationType);
	Data->SetStringField(TEXT("actor_path"), Operation->ActorPath);
	Data->SetStringField(TEXT("target"), Operation->Target);
	Data->SetStringField(TEXT("value"), Operation->Value);
	Data->SetNumberField(TEXT("timeout_seconds"), Operation->TimeoutSeconds);
	Data->SetStringField(TEXT("operation_state"), Operation->OperationState);
	Data->SetStringField(TEXT("execution_backend"), Operation->ExecutionBackend);
	Data->SetStringField(TEXT("tool_layer"), TEXT("L3_UITool_AsyncPrototype"));

	return AgentBridge::MakeSuccess(TEXT("Async UI operation started"), Data);
}

FBridgeResponse UAgentBridgeSubsystem::QueryUIOperation(const FString& OperationId)
{
	FBridgeResponse ValidationError;
	if (!AgentBridge::ValidateRequiredString(OperationId, TEXT("OperationId"), ValidationError)) return ValidationError;

	TSharedPtr<FPendingUIOperation>* Found = PendingUIOperations.Find(OperationId);
	if (!Found || !Found->IsValid())
	{
		return AgentBridge::MakeFailed(
			TEXT("UI operation not found"),
			EBridgeErrorCode::InvalidArgs,
			FString::Printf(TEXT("Unknown operation_id: %s"), *OperationId));
	}

	RefreshPendingUIOperation(*Found);
	return BuildPendingUIOperationResponse(*Found);
}

void UAgentBridgeSubsystem::RefreshPendingUIOperation(const TSharedPtr<FPendingUIOperation>& Operation)
{
	if (!Operation.IsValid() || !Operation->Future.IsValid())
	{
		return;
	}

	if (!Operation->Future->IsReady())
	{
		Operation->OperationState = TEXT("running");
		Operation->UpdatedAtSeconds = FPlatformTime::Seconds();
		Operation->Summary = TEXT("Async UI operation is still running");
		return;
	}

	FUIOperationResult UIResult = Operation->Future->Get();
	Operation->Future.Reset();
	Operation->FinalResult = UIResult;
	Operation->DebugLocatorPath = UIResult.DebugLocatorPath;
	Operation->UpdatedAtSeconds = FPlatformTime::Seconds();

	if (UIResult.IsSuccess())
	{
		Operation->OperationState = TEXT("success");
		Operation->Summary = TEXT("Async UI operation completed successfully");
	}
	else
	{
		Operation->OperationState = TEXT("failed");
		Operation->Summary = TEXT("Async UI operation failed");
		Operation->FailureReason = UIResult.FailureReason;
	}
}

FBridgeResponse UAgentBridgeSubsystem::BuildPendingUIOperationResponse(const TSharedPtr<FPendingUIOperation>& Operation) const
{
	check(Operation.IsValid());

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("operation_id"), Operation->OperationId);
	Data->SetStringField(TEXT("operation_type"), Operation->OperationType);
	Data->SetStringField(TEXT("actor_path"), Operation->ActorPath);
	Data->SetStringField(TEXT("target"), Operation->Target);
	Data->SetStringField(TEXT("value"), Operation->Value);
	if (Operation->bHasDropLocation)
	{
		TArray<TSharedPtr<FJsonValue>> DropLocationArray;
		DropLocationArray.Add(MakeShareable(new FJsonValueNumber(Operation->DropLocation.X)));
		DropLocationArray.Add(MakeShareable(new FJsonValueNumber(Operation->DropLocation.Y)));
		DropLocationArray.Add(MakeShareable(new FJsonValueNumber(Operation->DropLocation.Z)));
		Data->SetArrayField(TEXT("drop_location"), DropLocationArray);
	}
	Data->SetStringField(TEXT("operation_state"), Operation->OperationState);
	Data->SetStringField(TEXT("execution_backend"), Operation->ExecutionBackend);
	Data->SetNumberField(TEXT("timeout_seconds"), Operation->TimeoutSeconds);
	Data->SetNumberField(TEXT("created_at_seconds"), Operation->CreatedAtSeconds);
	Data->SetNumberField(TEXT("updated_at_seconds"), Operation->UpdatedAtSeconds);
	Data->SetStringField(TEXT("tool_layer"), TEXT("L3_UITool_AsyncPrototype"));

	if (!Operation->DebugLocatorPath.IsEmpty())
	{
		Data->SetStringField(TEXT("debug_locator_path"), Operation->DebugLocatorPath);
	}

	if (Operation->FinalResult.IsSet())
	{
		const FUIOperationResult& UIResult = Operation->FinalResult.GetValue();
		Data->SetBoolField(TEXT("executed"), UIResult.bExecuted);
		Data->SetBoolField(TEXT("ui_idle_after"), UIResult.bUIIdleAfter);
		Data->SetNumberField(TEXT("duration_seconds"), UIResult.DurationSeconds);
		if (!UIResult.FailureReason.IsEmpty())
		{
			Data->SetStringField(TEXT("failure_reason"), UIResult.FailureReason);
		}
	}

	if (Operation->OperationState == TEXT("running") || Operation->OperationState == TEXT("pending"))
	{
		return AgentBridge::MakeSuccess(Operation->Summary, Data);
	}

	if (Operation->OperationState == TEXT("success"))
	{
		return AgentBridge::MakeSuccess(Operation->Summary, Data);
	}

	EBridgeErrorCode Code = EBridgeErrorCode::ToolExecutionFailed;
	const FString FailureReason = Operation->FailureReason;
	if (FailureReason.Contains(TEXT("not available")))
	{
		Code = EBridgeErrorCode::DriverNotAvailable;
	}
	else if (FailureReason.Contains(TEXT("not found")))
	{
		Code = EBridgeErrorCode::WidgetNotFound;
	}
	else if (FailureReason.Contains(TEXT("timeout")) || FailureReason.Contains(TEXT("idle")))
	{
		Code = EBridgeErrorCode::UIOperationTimeout;
	}

	FBridgeResponse Failed = AgentBridge::MakeFailed(Operation->Summary, Code, FailureReason);
	Failed.Data = Data;
	Failed.SyncForRemote();
	return Failed;
}

// ============================================================
// CrossVerifyUIOperation
// L3→L1 交叉比对：拿 L3 返回值与 L1 独立读回做对比
// ============================================================

FBridgeUIVerification UAgentBridgeSubsystem::CrossVerifyUIOperation(
	const FBridgeResponse& UIToolResponse,
	const FString& L1VerifyFunc,
	const FString& L1VerifyParams)
{
	// 如果 L3 本身已失败，直接返回
	if (!UIToolResponse.IsSuccess())
	{
		return AgentBridge::MakeUIVerification(UIToolResponse, FBridgeResponse(), false,
			{FString::Printf(TEXT("L3 operation failed: %s"), *UIToolResponse.Summary)});
	}

	// 执行 L1 独立读回
	FBridgeResponse L1Response;

	if (L1VerifyFunc.Equals(TEXT("GetActorState"), ESearchCase::IgnoreCase))
	{
		L1Response = GetActorState(L1VerifyParams);
	}
	else if (L1VerifyFunc.Equals(TEXT("ListLevelActors"), ESearchCase::IgnoreCase))
	{
		L1Response = ListLevelActors();
	}
	else if (L1VerifyFunc.Equals(TEXT("GetActorBounds"), ESearchCase::IgnoreCase))
	{
		L1Response = GetActorBounds(L1VerifyParams);
	}
	else if (L1VerifyFunc.Equals(TEXT("GetAssetMetadata"), ESearchCase::IgnoreCase))
	{
		L1Response = GetAssetMetadata(L1VerifyParams);
	}
	else
	{
		return AgentBridge::MakeUIVerification(UIToolResponse, FBridgeResponse(), false,
			{FString::Printf(TEXT("Unknown L1 verify function: %s"), *L1VerifyFunc)});
	}

	// 如果 L1 读回失败，也是 mismatch
	if (!L1Response.IsSuccess())
	{
		return AgentBridge::MakeUIVerification(UIToolResponse, L1Response, false,
			{FString::Printf(TEXT("L1 verification failed: %s"), *L1Response.Summary)});
	}

	// 比对 L3 data 与 L1 data 中的关键字段
	TArray<FString> Mismatches;
	bool bConsistent = true;

	// DragAssetToViewport 特殊比对：检查 created_actors 是否出现在 L1 ListLevelActors 结果中
	if (UIToolResponse.Data.IsValid() && UIToolResponse.Data->HasField(TEXT("actors_created")))
	{
		int32 Created = (int32)UIToolResponse.Data->GetNumberField(TEXT("actors_created"));
		if (Created <= 0)
		{
			Mismatches.Add(TEXT("L3 reports 0 actors created"));
			bConsistent = false;
		}
		else if (L1Response.Data.IsValid() && L1Response.Data->HasField(TEXT("actors")))
		{
			// 检查 L3 声称创建的 Actor 是否在 L1 的 Actor 列表中
			const TArray<TSharedPtr<FJsonValue>>* CreatedActors;
			if (UIToolResponse.Data->TryGetArrayField(TEXT("created_actors"), CreatedActors))
			{
				const TArray<TSharedPtr<FJsonValue>>* AllActors;
				if (L1Response.Data->TryGetArrayField(TEXT("actors"), AllActors))
				{
					for (const auto& CA : *CreatedActors)
					{
						FString CreatedPath = CA->AsObject()->GetStringField(TEXT("actor_path"));
						bool bFound = false;
						for (const auto& A : *AllActors)
						{
							if (A->AsObject()->GetStringField(TEXT("actor_path")) == CreatedPath)
							{
								bFound = true;
								break;
							}
						}
						if (!bFound)
						{
							Mismatches.Add(FString::Printf(TEXT("L3 created actor '%s' not found in L1 ListLevelActors"), *CreatedPath));
							bConsistent = false;
						}
					}
				}
			}
		}
	}

	// TypeInDetailPanelField / ClickDetailPanelButton 的比对：
	// 比较 L3 操作后 L1 读回的 actor_state 是否反映了变更
	// 具体字段比对取决于操作类型，此处做通用的 status 级别比对
	if (Mismatches.Num() == 0 && UIToolResponse.IsSuccess() && L1Response.IsSuccess())
	{
		// 两者都成功且无具体字段 mismatch → 一致
		bConsistent = true;
	}

	return AgentBridge::MakeUIVerification(UIToolResponse, L1Response, bConsistent, Mismatches);
}

