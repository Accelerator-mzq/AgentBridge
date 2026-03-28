// L2_UIToolClosedLoopSpec.spec.cpp
// AGENT + UE5 可操作层 — L2 闭环验证：L3 UI 工具
//
// 目标：
//   1. 验证 DragAssetToViewport 的 L3 执行 → L1 ListLevelActors / GetActorState 闭环
//   2. 验证 TypeInDetailPanelField 的 L3 执行后，L1 读回仍然可用且值正确
//
// 说明：
//   - Driver 不可用时，Spec 以 warning 方式跳过，不阻塞 CI
//   - 每个 It 都依赖 BeforeEach 准备独立环境，AfterEach 负责 Undo 清理

#include "Misc/AutomationTest.h"
#include "AgentBridgeSubsystem.h"
#include "AutomationDriverAdapter.h"
#include "BridgeTypes.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	static UAgentBridgeSubsystem* GetUIToolSpecSubsystem(FAutomationTestBase& Test)
	{
		UAgentBridgeSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>() : nullptr;
		if (!Subsystem)
		{
			Test.AddError(TEXT("AgentBridgeSubsystem 不可用，请确认 AgentBridge 插件已启用。"));
		}
		return Subsystem;
	}

	static FString GetCurrentLevelPath_UIToolSpec()
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

	static int32 GetActorCountFromListResponse(const FBridgeResponse& Response)
	{
		if (!Response.Data.IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (!Response.Data->TryGetArrayField(TEXT("actors"), Actors) || !Actors)
		{
			return 0;
		}

		return Actors->Num();
	}

	static FString GetCreatedActorPathFromDragResponse(const FBridgeResponse& Response)
	{
		if (!Response.Data.IsValid())
		{
			return FString();
		}

		const TArray<TSharedPtr<FJsonValue>>* CreatedActors = nullptr;
		if (!Response.Data->TryGetArrayField(TEXT("created_actors"), CreatedActors) || !CreatedActors || CreatedActors->Num() == 0)
		{
			return FString();
		}

		const TSharedPtr<FJsonObject> First = (*CreatedActors)[0].IsValid() ? (*CreatedActors)[0]->AsObject() : nullptr;
		if (!First.IsValid() || !First->HasTypedField<EJson::String>(TEXT("actor_path")))
		{
			return FString();
		}

		return First->GetStringField(TEXT("actor_path"));
	}

	static FString GetCreatedActorPathFromSpawnResponse(const FBridgeResponse& Response)
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

		const TSharedPtr<FJsonObject> First = (*CreatedObjects)[0].IsValid() ? (*CreatedObjects)[0]->AsObject() : nullptr;
		if (!First.IsValid() || !First->HasTypedField<EJson::String>(TEXT("actor_path")))
		{
			return FString();
		}

		return First->GetStringField(TEXT("actor_path"));
	}

	static bool TryReadTransformArray(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& TransformObject,
		const TCHAR* FieldName,
		double& OutA,
		double& OutB,
		double& OutC)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!TransformObject.IsValid() || !TransformObject->TryGetArrayField(FieldName, Values) || !Values || Values->Num() != 3)
		{
			Test.AddError(FString::Printf(TEXT("字段 %s 缺失或格式错误。"), FieldName));
			return false;
		}

		OutA = (*Values)[0]->AsNumber();
		OutB = (*Values)[1]->AsNumber();
		OutC = (*Values)[2]->AsNumber();
		return true;
	}

	static TSharedPtr<FJsonObject> GetTransformObjectFromActorState(
		FAutomationTestBase& Test,
		const FBridgeResponse& Response)
	{
		if (!Response.Data.IsValid())
		{
			Test.AddError(TEXT("ActorState 响应缺少 data。"));
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* TransformObject = nullptr;
		if (!Response.Data->TryGetObjectField(TEXT("transform"), TransformObject) || !TransformObject || !(*TransformObject).IsValid())
		{
			Test.AddError(TEXT("ActorState 响应缺少 transform。"));
			return nullptr;
		}

		return *TransformObject;
	}

	static FVector GetStableUIToolDropLocation_L2Spec()
	{
		// 优先取当前视口中心射线落到地面 Z=0 的点，保证 Drag 目标位于当前视野中。
		if (GCurrentLevelEditingViewportClient)
		{
			const FVector ViewLocation = GCurrentLevelEditingViewportClient->GetViewLocation();
			const FVector ViewDirection = GCurrentLevelEditingViewportClient->GetViewRotation().Vector();

			if (!FMath::IsNearlyZero(ViewDirection.Z))
			{
				const double RayDistance = -ViewLocation.Z / ViewDirection.Z;
				if (RayDistance > 100.0)
				{
					FVector VisiblePoint = ViewLocation + ViewDirection * RayDistance;
					VisiblePoint.Z = 0.0f;
					return VisiblePoint;
				}
			}
		}

		// 回退到已在 Task14 真机验证中过的保守点。
		return FVector(600.0f, 400.0f, 0.0f);
	}
}

BEGIN_DEFINE_SPEC(
	FBridgeL2_DragAssetToViewportLoop,
	"Project.AgentBridge.L2.UITool.DragAssetToViewportLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)
	UAgentBridgeSubsystem* Subsystem = nullptr;
	bool bDriverAvailable = false;
	int32 ActorCountBefore = 0;
	FBridgeResponse DragResponse;
	FString CreatedActorPath;
	FVector RequestedDropLocation = FVector(600.0f, 400.0f, 0.0f);
	bool bUndoHandled = false;
END_DEFINE_SPEC(FBridgeL2_DragAssetToViewportLoop)

void FBridgeL2_DragAssetToViewportLoop::Define()
{
	Describe(TEXT("drag asset to viewport then verify via L1 tools"), [this]()
	{
		BeforeEach([this]()
		{
			Subsystem = GetUIToolSpecSubsystem(*this);
			bDriverAvailable = (Subsystem != nullptr) && Subsystem->IsAutomationDriverAvailable();
			bUndoHandled = false;
			ActorCountBefore = 0;
			CreatedActorPath.Reset();
			DragResponse = FBridgeResponse();
			RequestedDropLocation = GetStableUIToolDropLocation_L2Spec();

			if (!Subsystem)
			{
				return;
			}

			if (!bDriverAvailable)
			{
				AddWarning(TEXT("Automation Driver 不可用，L2.UITool.DragAssetToViewportLoop 本轮跳过。"));
				return;
			}

			const FBridgeResponse ListBefore = Subsystem->ListLevelActors();
			if (!ListBefore.IsSuccess())
			{
				AddError(FString::Printf(TEXT("BeforeEach 读取 Actor 列表失败：%s"), *ListBefore.Summary));
				return;
			}

			ActorCountBefore = GetActorCountFromListResponse(ListBefore);
			DragResponse = Subsystem->DragAssetToViewport(TEXT("/Engine/BasicShapes/Cube"), RequestedDropLocation);
			CreatedActorPath = GetCreatedActorPathFromDragResponse(DragResponse);
		});

		It(TEXT("should execute successfully with L3 tool_layer"), [this]()
		{
			if (!bDriverAvailable || !Subsystem)
			{
				return;
			}

			TestTrue(TEXT("DragAssetToViewport 应成功执行"), DragResponse.IsSuccess());
			TestTrue(TEXT("tool_layer 应为 L3_UITool"),
				DragResponse.Data.IsValid()
				&& DragResponse.Data->HasTypedField<EJson::String>(TEXT("tool_layer"))
				&& DragResponse.Data->GetStringField(TEXT("tool_layer")) == TEXT("L3_UITool"));
		});

		It(TEXT("should increase actor count via ListLevelActors"), [this]()
		{
			if (!bDriverAvailable || !Subsystem)
			{
				return;
			}

			const FBridgeResponse ListAfter = Subsystem->ListLevelActors();
			TestTrue(TEXT("ListLevelActors after drag 应成功"), ListAfter.IsSuccess());
			const int32 ActorCountAfter = GetActorCountFromListResponse(ListAfter);
			TestTrue(TEXT("Actor 数量应增加"), ActorCountAfter > ActorCountBefore);
		});

		It(TEXT("should report L3 to L1 cross verification consistent"), [this]()
		{
			if (!bDriverAvailable || !Subsystem)
			{
				return;
			}

			const FBridgeUIVerification Verification = Subsystem->CrossVerifyUIOperation(
				DragResponse,
				TEXT("ListLevelActors"),
				TEXT(""));

			TestTrue(TEXT("CrossVerifyUIOperation 应 consistent=true"), Verification.bConsistent);
			TestEqual(TEXT("CrossVerify final_status 应为 success"),
				BridgeStatusToString(Verification.GetFinalStatus()),
				TEXT("success"));
		});

		It(TEXT("should place created actor near requested drop location"), [this]()
		{
			if (!bDriverAvailable || !Subsystem)
			{
				return;
			}

			TestFalse(TEXT("created actor path 不应为空"), CreatedActorPath.IsEmpty());
			if (CreatedActorPath.IsEmpty())
			{
				return;
			}

			const FBridgeResponse ActorState = Subsystem->GetActorState(CreatedActorPath);
			TestTrue(TEXT("GetActorState 应成功"), ActorState.IsSuccess());

			const TSharedPtr<FJsonObject> TransformObject = GetTransformObjectFromActorState(*this, ActorState);
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!TransformObject.IsValid()
				|| !TryReadTransformArray(*this, TransformObject, TEXT("location"), X, Y, Z))
			{
				return;
			}

			TestNearlyEqual(TEXT("Location.X tolerance 100cm"), X, RequestedDropLocation.X, 100.0);
			TestNearlyEqual(TEXT("Location.Y tolerance 100cm"), Y, RequestedDropLocation.Y, 100.0);
			TestNearlyEqual(TEXT("Location.Z tolerance 100cm"), Z, RequestedDropLocation.Z, 100.0);
		});

		It(TEXT("should be undoable and restore actor count"), [this]()
		{
			if (!bDriverAvailable || !Subsystem)
			{
				return;
			}

			if (GEditor)
			{
				GEditor->UndoTransaction();
				bUndoHandled = true;
			}

			const FBridgeResponse ListAfterUndo = Subsystem->ListLevelActors();
			TestTrue(TEXT("Undo 后 ListLevelActors 应成功"), ListAfterUndo.IsSuccess());
			const int32 ActorCountAfterUndo = GetActorCountFromListResponse(ListAfterUndo);
			TestEqual(TEXT("Undo 后 Actor 数量应恢复"), ActorCountAfterUndo, ActorCountBefore);
		});

		AfterEach([this]()
		{
			if (bDriverAvailable && !bUndoHandled && GEditor)
			{
				GEditor->UndoTransaction();
			}
		});
	});
}

BEGIN_DEFINE_SPEC(
	FBridgeL2_TypeInFieldLoop,
	"Project.AgentBridge.L2.UITool.TypeInFieldLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)
	UAgentBridgeSubsystem* Subsystem = nullptr;
	bool bDriverAvailable = false;
	FString LevelPath;
	FString ActorPath;
	FBridgeResponse SpawnResponse;
	FBridgeResponse BaselineState;
	FBridgeResponse TypeResponse;
	bool bTypeExecuted = false;
END_DEFINE_SPEC(FBridgeL2_TypeInFieldLoop)

void FBridgeL2_TypeInFieldLoop::Define()
{
	Describe(TEXT("type in detail panel field then verify via L1 readback"), [this]()
	{
		BeforeEach([this]()
		{
			Subsystem = GetUIToolSpecSubsystem(*this);
			bDriverAvailable = (Subsystem != nullptr) && Subsystem->IsAutomationDriverAvailable();
			LevelPath = GetCurrentLevelPath_UIToolSpec();
			ActorPath.Reset();
			SpawnResponse = FBridgeResponse();
			BaselineState = FBridgeResponse();
			TypeResponse = FBridgeResponse();
			bTypeExecuted = false;

			if (!Subsystem)
			{
				return;
			}

			if (!bDriverAvailable)
			{
				AddWarning(TEXT("Automation Driver 不可用，L2.UITool.TypeInFieldLoop 本轮跳过。"));
				return;
			}

			FBridgeTransform SpawnTransform;
			SpawnTransform.Location = FVector(111.0f, 222.0f, 333.0f);
			SpawnTransform.Rotation = FRotator::ZeroRotator;
			SpawnTransform.RelativeScale3D = FVector::OneVector;

			SpawnResponse = Subsystem->SpawnActor(
				LevelPath,
				TEXT("/Script/Engine.StaticMeshActor"),
				TEXT("L2_UITool_TypeField"),
				SpawnTransform,
				false);

			if (!SpawnResponse.IsSuccess())
			{
				AddError(FString::Printf(TEXT("BeforeEach SpawnActor 失败：%s"), *SpawnResponse.Summary));
				return;
			}

			ActorPath = GetCreatedActorPathFromSpawnResponse(SpawnResponse);
			if (ActorPath.IsEmpty())
			{
				AddError(TEXT("SpawnActor 未返回 actor_path。"));
				return;
			}

			BaselineState = Subsystem->GetActorState(ActorPath);
			if (!BaselineState.IsSuccess())
			{
				AddError(FString::Printf(TEXT("Baseline GetActorState 失败：%s"), *BaselineState.Summary));
			}
		});

		It(TEXT("should keep baseline readback available before typing"), [this]()
		{
			if (!bDriverAvailable || !Subsystem)
			{
				return;
			}

			TestTrue(TEXT("Baseline GetActorState 应成功"), BaselineState.IsSuccess());

			const TSharedPtr<FJsonObject> TransformObject = GetTransformObjectFromActorState(*this, BaselineState);
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!TransformObject.IsValid()
				|| !TryReadTransformArray(*this, TransformObject, TEXT("location"), X, Y, Z))
			{
				return;
			}

			TestNearlyEqual(TEXT("Baseline X"), X, 111.0, 0.01);
			TestNearlyEqual(TEXT("Baseline Y"), Y, 222.0, 0.01);
			TestNearlyEqual(TEXT("Baseline Z"), Z, 333.0, 0.01);
		});

		It(TEXT("should execute type operation successfully with L3 tool_layer"), [this]()
		{
			if (!bDriverAvailable || !Subsystem)
			{
				return;
			}

			TypeResponse = Subsystem->TypeInDetailPanelField(
				ActorPath,
				TEXT("RelativeLocation.X"),
				TEXT("1337.5"),
				false);
			bTypeExecuted = TypeResponse.IsSuccess();

			TestTrue(TEXT("TypeInDetailPanelField 应成功"), TypeResponse.IsSuccess());
			TestTrue(TEXT("tool_layer 应为 L3_UITool"),
				TypeResponse.Data.IsValid()
				&& TypeResponse.Data->HasTypedField<EJson::String>(TEXT("tool_layer"))
				&& TypeResponse.Data->GetStringField(TEXT("tool_layer")) == TEXT("L3_UITool"));
		});

		It(TEXT("should keep L1 readback usable after typing and read updated value"), [this]()
		{
			if (!bDriverAvailable || !Subsystem)
			{
				return;
			}

			TypeResponse = Subsystem->TypeInDetailPanelField(
				ActorPath,
				TEXT("RelativeLocation.X"),
				TEXT("1337.5"),
				false);
			bTypeExecuted = TypeResponse.IsSuccess();
			TestTrue(TEXT("TypeInDetailPanelField 应成功"), TypeResponse.IsSuccess());

			const FBridgeResponse ActorStateAfterType = Subsystem->GetActorState(ActorPath);
			TestTrue(TEXT("L1 GetActorState 在 L3 后仍应可用"), ActorStateAfterType.IsSuccess());

			const TSharedPtr<FJsonObject> TransformObject = GetTransformObjectFromActorState(*this, ActorStateAfterType);
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!TransformObject.IsValid()
				|| !TryReadTransformArray(*this, TransformObject, TEXT("location"), X, Y, Z))
			{
				return;
			}

			TestNearlyEqual(TEXT("Updated X"), X, 1337.5, 0.01);
			TestNearlyEqual(TEXT("Updated Y should remain"), Y, 222.0, 0.01);
			TestNearlyEqual(TEXT("Updated Z should remain"), Z, 333.0, 0.01);
		});

		AfterEach([this]()
		{
			if (!bDriverAvailable || !GEditor)
			{
				return;
			}

			// L3 输入通常会创建一个属性修改事务；随后还需要撤销 SpawnActor。
			GEditor->UndoTransaction();
			GEditor->UndoTransaction();
		});
	});
}

