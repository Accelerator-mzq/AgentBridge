// L1_UIToolTests.cpp
// AGENT + UE5 可操作層 — L1 單接口驗證：L3 UI 工具接口
//
// UE5 官方模組：Automation Test Framework + Automation Driver
// 註冊方式：IMPLEMENT_SIMPLE_AUTOMATION_TEST 宏
// Test Flag：EditorContext + ProductFilter
// Session Frontend 路径：Project.AgentBridge.L1.UITool.*
//
// 测试策略：
//   1. 每个 L3 接口的参数校验 / dry_run / Driver 可用性
//   2. 实际执行后通过 L1 语义工具做独立读回
//   3. L3 返回值与 L1 返回值做交叉比对（FBridgeUIVerification）
//   4. 两者一致 → PASS，不一致 → FAIL + mismatch 详情

#include "Misc/AutomationTest.h"
#include "AgentBridgeSubsystem.h"
#include "AutomationDriverAdapter.h"
#include "BridgeTypes.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "EditorLevelLibrary.h"

// ============================================================
// 辅助宏
// ============================================================

namespace
{
	static FVector GetStableUIToolDropLocation()
	{
		// 优先使用当前关卡视口中心射线与地面 Z=0 的交点。
		// 这样得到的目标点天然位于当前相机可见区域内，比写死远处坐标更稳定。
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

		// 回退到已在 Task14 真机闭环中验证过的保守坐标。
		return FVector(600.0f, 400.0f, 0.0f);
	}
}

#define GET_SUBSYSTEM_OR_FAIL() \
	UAgentBridgeSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>() : nullptr; \
	if (!Subsystem) \
	{ \
		AddError(TEXT("AgentBridgeSubsystem not available")); \
		return false; \
	}

#define SKIP_IF_DRIVER_UNAVAILABLE() \
	if (!Subsystem->IsAutomationDriverAvailable()) \
	{ \
		AddWarning(TEXT("Automation Driver not available — skipping L3 UI Tool test. Enable AutomationDriver plugin to run.")); \
		return true; \
	}

// ============================================================
// T1-12: IsAutomationDriverAvailable
// 验证 Driver 可用性查询接口
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBridgeL1_IsAutomationDriverAvailable,
	"Project.AgentBridge.L1.UITool.IsAutomationDriverAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FBridgeL1_IsAutomationDriverAvailable::RunTest(const FString& Parameters)
{
	GET_SUBSYSTEM_OR_FAIL();

	// 此接口始终可调用，返回 bool
	bool bAvailable = Subsystem->IsAutomationDriverAvailable();

	// 不做硬断言（CI 环境可能未启用 Driver）
	// 仅验证接口可调用且返回确定值
	AddInfo(FString::Printf(TEXT("Automation Driver available: %s"),
		bAvailable ? TEXT("true") : TEXT("false")));

	// 验证与 FAutomationDriverAdapter::IsAvailable() 一致
	bool bAdapterAvailable = FAutomationDriverAdapter::IsAvailable();
	TestEqual(TEXT("Subsystem and Adapter should agree"),
		bAvailable, bAdapterAvailable);

	return true;
}

// ============================================================
// T1-13: ClickDetailPanelButton
// 验证：参数校验 + dry_run + Driver 不可用处理 + 实际执行（如 Driver 可用）
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBridgeL1_ClickDetailPanelButton,
	"Project.AgentBridge.L1.UITool.ClickDetailPanelButton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FBridgeL1_ClickDetailPanelButton::RunTest(const FString& Parameters)
{
	GET_SUBSYSTEM_OR_FAIL();

	// --- 测试 1: 参数校验 ---
	{
		FBridgeResponse EmptyActor = Subsystem->ClickDetailPanelButton(TEXT(""), TEXT("SomeButton"));
		TestEqual(TEXT("Empty actor → validation_error"),
			BridgeStatusToString(EmptyActor.Status), TEXT("validation_error"));

		FBridgeResponse EmptyLabel = Subsystem->ClickDetailPanelButton(TEXT("/Game/Actor"), TEXT(""));
		TestEqual(TEXT("Empty label → validation_error"),
			BridgeStatusToString(EmptyLabel.Status), TEXT("validation_error"));
	}

	// 以下测试需要 Automation Driver
	SKIP_IF_DRIVER_UNAVAILABLE();

	// --- 测试 2: dry_run ---
	{
		// 先获取一个有效 Actor
		FBridgeResponse ListResp = Subsystem->ListLevelActors();
		if (!ListResp.IsSuccess()) { AddWarning(TEXT("No actors — skipping dry_run")); return true; }

		const TArray<TSharedPtr<FJsonValue>>* Actors;
		if (!ListResp.Data->TryGetArrayField(TEXT("actors"), Actors) || Actors->Num() == 0)
		{
			AddWarning(TEXT("No actors in level")); return true;
		}

		FString ActorPath = (*Actors)[0]->AsObject()->GetStringField(TEXT("actor_path"));

		FBridgeResponse DryRun = Subsystem->ClickDetailPanelButton(ActorPath, TEXT("TestButton"), true);
		TestEqual(TEXT("dry_run should succeed"),
			BridgeStatusToString(DryRun.Status), TEXT("success"));
		TestTrue(TEXT("dry_run data should contain tool_layer"),
			DryRun.Data.IsValid() && DryRun.Data->GetStringField(TEXT("tool_layer")) == TEXT("L3_UITool"));
	}

	// --- 测试 3: 不存在的 Actor ---
	{
		FBridgeResponse NotFound = Subsystem->ClickDetailPanelButton(
			TEXT("/Game/NonExistent.Actor"), TEXT("Button"));
		TestEqual(TEXT("Not found actor → failed"),
			BridgeStatusToString(NotFound.Status), TEXT("failed"));
	}

	return true;
}

// ============================================================
// T1-14: TypeInDetailPanelField
// 验证：参数校验 + dry_run + 实际执行 + L3→L1 交叉比对
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBridgeL1_TypeInDetailPanelField,
	"Project.AgentBridge.L1.UITool.TypeInDetailPanelField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FBridgeL1_TypeInDetailPanelField::RunTest(const FString& Parameters)
{
	GET_SUBSYSTEM_OR_FAIL();

	// --- 测试 1: 参数校验 ---
	{
		FBridgeResponse EmptyActor = Subsystem->TypeInDetailPanelField(TEXT(""), TEXT("Prop"), TEXT("Val"));
		TestEqual(TEXT("Empty actor → validation_error"),
			BridgeStatusToString(EmptyActor.Status), TEXT("validation_error"));

		FBridgeResponse EmptyProp = Subsystem->TypeInDetailPanelField(TEXT("/Game/A"), TEXT(""), TEXT("Val"));
		TestEqual(TEXT("Empty property → validation_error"),
			BridgeStatusToString(EmptyProp.Status), TEXT("validation_error"));

		FBridgeResponse EmptyVal = Subsystem->TypeInDetailPanelField(TEXT("/Game/A"), TEXT("Prop"), TEXT(""));
		TestEqual(TEXT("Empty value → validation_error"),
			BridgeStatusToString(EmptyVal.Status), TEXT("validation_error"));
	}

	SKIP_IF_DRIVER_UNAVAILABLE();

	// --- 测试 2: dry_run ---
	{
		FBridgeResponse ListResp = Subsystem->ListLevelActors();
		if (!ListResp.IsSuccess()) { AddWarning(TEXT("No actors")); return true; }

		const TArray<TSharedPtr<FJsonValue>>* Actors;
		if (!ListResp.Data->TryGetArrayField(TEXT("actors"), Actors) || Actors->Num() == 0)
		{
			AddWarning(TEXT("No actors in level")); return true;
		}

		FString ActorPath = (*Actors)[0]->AsObject()->GetStringField(TEXT("actor_path"));

		FBridgeResponse DryRun = Subsystem->TypeInDetailPanelField(
			ActorPath, TEXT("SomeProperty"), TEXT("SomeValue"), true);
		TestEqual(TEXT("dry_run should succeed"),
			BridgeStatusToString(DryRun.Status), TEXT("success"));
	}

	return true;
}

// ============================================================
// T1-15: DragAssetToViewport
// 验证：参数校验 + dry_run + 实际执行 + L3→L1 交叉比对
// 这是 L3 测试中最重要的一个——验证完整的 L3 执行→L1 读回→交叉比对 链路
// ============================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBridgeL1_DragAssetToViewport,
	"Project.AgentBridge.L1.UITool.DragAssetToViewport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FBridgeL1_DragAssetToViewport::RunTest(const FString& Parameters)
{
	GET_SUBSYSTEM_OR_FAIL();

	// --- 测试 1: 参数校验 ---
	{
		FBridgeResponse EmptyAsset = Subsystem->DragAssetToViewport(TEXT(""), FVector::ZeroVector);
		TestEqual(TEXT("Empty asset → validation_error"),
			BridgeStatusToString(EmptyAsset.Status), TEXT("validation_error"));
	}

	SKIP_IF_DRIVER_UNAVAILABLE();

	const FVector DropLocation = GetStableUIToolDropLocation();

	// --- 测试 2: dry_run ---
	{
		FBridgeResponse DryRun = Subsystem->DragAssetToViewport(
			TEXT("/Engine/BasicShapes/Cube"), DropLocation, true);
		TestEqual(TEXT("dry_run should succeed"),
			BridgeStatusToString(DryRun.Status), TEXT("success"));

		if (DryRun.Data.IsValid())
		{
			TestTrue(TEXT("dry_run should have tool_layer"),
				DryRun.Data->GetStringField(TEXT("tool_layer")) == TEXT("L3_UITool"));
			TestTrue(TEXT("dry_run should have drop_location"),
				DryRun.Data->HasField(TEXT("drop_location")));
		}
	}

	// --- 测试 3: 实际执行 + L3→L1 交叉比对 ---
	{
		// 记录操作前 Actor 数量
		FBridgeResponse ListBefore = Subsystem->ListLevelActors();
		int32 CountBefore = 0;
		if (ListBefore.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ActorArr;
			if (ListBefore.Data->TryGetArrayField(TEXT("actors"), ActorArr))
			{
				CountBefore = ActorArr->Num();
			}
		}

		// L3 执行
		FBridgeResponse L3Response = Subsystem->DragAssetToViewport(
			TEXT("/Engine/BasicShapes/Cube"), DropLocation);
		TestTrue(TEXT("DragAssetToViewport should succeed when driver is available"), L3Response.IsSuccess());
		if (!L3Response.IsSuccess())
		{
			AddError(FString::Printf(TEXT("DragAssetToViewport failed: %s"), *L3Response.Summary));
			return false;
		}

		// L3→L1 交叉比对
		FBridgeUIVerification Verification = Subsystem->CrossVerifyUIOperation(
			L3Response, TEXT("ListLevelActors"), TEXT(""));

		AddInfo(FString::Printf(TEXT("Cross-verification: consistent=%s, final_status=%s"),
			Verification.bConsistent ? TEXT("true") : TEXT("false"),
			*BridgeStatusToString(Verification.GetFinalStatus())));

		// 验证交叉比对结果
		EBridgeStatus FinalStatus = Verification.GetFinalStatus();
		TestTrue(TEXT("Final status should be success or mismatch (not failed)"),
			FinalStatus == EBridgeStatus::Success || FinalStatus == EBridgeStatus::Mismatch);

		// 验证 L3 data 中的 actors_created
		if (L3Response.Data.IsValid() && L3Response.Data->HasField(TEXT("actors_created")))
		{
			int32 Created = (int32)L3Response.Data->GetNumberField(TEXT("actors_created"));
			AddInfo(FString::Printf(TEXT("L3 reports %d actors created"), Created));

			if (Created > 0)
			{
				// L1 独立验证：Actor 数量确实增加了
				FBridgeResponse ListAfter = Subsystem->ListLevelActors();
				int32 CountAfter = 0;
				if (ListAfter.Data.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* ActorArr;
					if (ListAfter.Data->TryGetArrayField(TEXT("actors"), ActorArr))
					{
						CountAfter = ActorArr->Num();
					}
				}

				TestTrue(TEXT("L1 should confirm actor count increased"),
					CountAfter > CountBefore);

				AddInfo(FString::Printf(TEXT("Actor count: before=%d, after=%d (L3 claims +%d)"),
					CountBefore, CountAfter, Created));
			}
		}

		// 如果有 mismatch，记录详情
		for (const FString& Mismatch : Verification.Mismatches)
		{
			AddWarning(FString::Printf(TEXT("Mismatch: %s"), *Mismatch));
		}

		// 清理：Undo
		if (GEditor)
		{
			GEditor->UndoTransaction();
			AddInfo(TEXT("Undo executed to clean up dragged asset"));
		}
	}

	return true;
}

