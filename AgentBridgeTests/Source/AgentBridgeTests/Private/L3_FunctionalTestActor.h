// L3_FunctionalTestActor.h
// AGENT + UE5 可操作層 — L3 完整 Demo 验证
//
// UE5 官方模組：Functional Testing
// 基类：AFunctionalTest
// 放置在 FTEST_ 前缀的测试地图中（如 FTEST_WarehouseDemo）
//
// L3 与 L1/L2 的区别：
//   L1 = 单接口（不需要特定地图）
//   L2 = 闭环（不需要特定地图，BeforeEach 自行创建测试 Actor）
//   L3 = 完整场景（在专用测试地图中，执行完整的 Spec 流程并验证全部 Actor）
//
// 使用方式：
//   1. 创建 FTEST_WarehouseDemo 测试地图
//   2. 在地图中放置 AAgentBridgeFunctionalTest Actor
//   3. 设置 SpecPath 属性指向 Spec 文件
//   4. Session Frontend → Run Level Test → 自动识别 FTEST_ 地图
//   5. 或通过 Commandlet: -run=AgentBridge -RunTests="Project.AgentBridge.L3"
//
// Test Flag：EditorContext + ProductFilter

#pragma once

#include "CoreMinimal.h"
#include "FunctionalTest.h"
#include "BridgeTypes.h"
#include "L3_FunctionalTestActor.generated.h"

class UAgentBridgeSubsystem;
class AActor;

/**
 * L3 完整 Demo 验证 — Functional Test Actor
 * 
 * 在测试地图中执行完整的 Spec 流程：
 * spawn 多个 Actor → 逐个 readback → 容差验证 → 判定 Pass/Fail
 */
UCLASS(Blueprintable)
class AAgentBridgeFunctionalTest : public AFunctionalTest
{
	GENERATED_BODY()

public:
	AAgentBridgeFunctionalTest();

	// ============================================================
	// 可配置属性（在 Editor 中设置）
	// ============================================================

	/** Spec 文件路径（相对于项目根目录）。留空则使用内置测试场景。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBridge Test")
	FString SpecPath;

	/** 验证容差：Location（cm） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBridge Test")
	float LocationTolerance = 0.01f;

	/** 验证容差：Rotation（degrees） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBridge Test")
	float RotationTolerance = 0.01f;

	/** 验证容差：Scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBridge Test")
	float ScaleTolerance = 0.001f;

	/** 是否在测试完成后 Undo 全部操作 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBridge Test")
	bool bUndoAfterTest = true;

	/** 内置测试场景的 Actor 数量（SpecPath 为空时使用） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBridge Test")
	int32 BuiltInActorCount = 5;

protected:
	// ============================================================
	// AFunctionalTest 接口
	// ============================================================

	virtual void PrepareTest() override;
	virtual void StartTest() override;
	virtual void CleanUp() override;

	virtual bool IsReady_Implementation() override;
	virtual FString GetAdditionalTestFinishedMessage(EFunctionalTestResult TestResult) const override;

private:
	// ============================================================
	// 内部方法
	// ============================================================

	/** 执行内置测试场景（SpecPath 为空时） */
	void RunBuiltInScenario();

	/** 执行 Spec 文件驱动的测试 */
	void RunSpecDriven();

	/** 生成单个测试 Actor 并验证 */
	bool SpawnAndVerifyActor(
		int32 Index,
		const FString& ActorClass,
		const FString& ActorName,
		const FBridgeTransform& ExpectedTransform,
		TArray<FString>& OutSpawnedPaths
	);

	/** 执行全局验证（MapCheck + DirtyAssets） */
	void RunGlobalValidation(const TArray<FString>& SpawnedPaths);

	/** 记录测试报告 */
	void LogTestReport(int32 TotalActors, int32 PassedActors, int32 FailedActors);

	/** 读取当前 Undo 栈中已提交事务深度 */
	int32 GetCommittedTransactionDepth() const;

	/** 统一生成最终附加消息 */
	FString BuildFinishedMessage(const FString& CoreMessage) const;

	/** 跟踪已生成的 Actor 路径（用于清理） */
	TArray<FString> SpawnedActorPaths;

	/** 跟踪运行时生成的 Actor 引用（PIE 模式下直接 Destroy 清理） */
	TArray<TWeakObjectPtr<AActor>> SpawnedActorRefs;

	/** PrepareTest 时记录的事务深度，用于 CleanUp 做快照差值 Undo */
	int32 TransactionDepthBeforeTest = 0;

	/** 最近一次生成的报告路径（Spec 驱动模式） */
	FString LastReportPath;

	/** 最近一次的执行摘要 */
	FString LastExecutionSummary;

	/** 缓存 Subsystem 引用 */
	TObjectPtr<UAgentBridgeSubsystem> CachedSubsystem = nullptr;
};

