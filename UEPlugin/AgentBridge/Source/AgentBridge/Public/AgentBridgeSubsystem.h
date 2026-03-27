// AgentBridgeSubsystem.h
// AGENT + UE5 可操作层 — Bridge 封装层核心 Subsystem
//
// UAgentBridgeSubsystem 是 AgentBridge Plugin 的核心类：
//   - 继承 UEditorSubsystem：随 Editor 启动自动实例化
//   - L1 语义工具：15 个 Bridge 接口（查询 7 + 写入 4 + 验证 3 + 构建 1）
//   - L3 UI 工具：3 个 Automation Driver 接口（仅当 L1 无法覆盖时使用）
//   - 所有接口标记为 BlueprintCallable → Remote Control API 自动暴露
//   - L1 写接口使用 FScopedTransaction 管理 Undo/Redo
//   - L3 操作后通过 L1 做交叉比对验证（FBridgeUIVerification）
//   - 返回 FBridgeResponse 统一响应外壳
//
// 获取方式：
//   C++:    GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>()
//   Python: unreal.get_editor_subsystem(unreal.AgentBridgeSubsystem)
//   RC API: PUT /remote/object/call { objectPath: 默认对象路径, functionName: "..." }
//
// UE5 官方能力依赖：
//   - UEditorLevelLibrary / UEditorAssetLibrary（查询/写入）— L1
//   - AActor / UWorld / ULevel（引擎核心）— L1
//   - UPrimitiveComponent（碰撞）— L1
//   - FAssetData / UAssetToolsHelpers（资产导入）— L1
//   - UBlueprintFactory（Blueprint 创建）— L1
//   - FScopedTransaction（Undo/Redo）— L1
//   - Console Command "MAP CHECK"（地图检查）— L2
//   - Automation Driver / IAutomationDriverModule（UI 输入模拟）— L3

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "EditorSubsystem.h"
#include "BridgeTypes.h"
#include "AgentBridgeSubsystem.generated.h"

// L3 UI 工具层的操作结果（前向声明，完整定义在 AutomationDriverAdapter.h）
struct FUIOperationResult;
class FPendingUIOperation;

UCLASS()
class AGENTBRIDGE_API UAgentBridgeSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	// ============================================================
	// 生命周期
	// ============================================================

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ============================================================
	// 查询接口（7 个反馈接口）
	// UE5 官方能力：UEditorLevelLibrary / AActor / UEditorAssetLibrary
	// ============================================================

	/**
	 * 获取当前项目与编辑器上下文。
	 * UE5 依赖：FPaths + UKismetSystemLibrary::GetEngineVersion() + UEditorLevelLibrary::GetEditorWorld()
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse GetCurrentProjectState();

	/**
	 * 列出当前关卡中的全部 Actor。
	 * UE5 依赖：UEditorLevelLibrary::GetAllLevelActors()
	 * @param ClassFilter 可选的类名过滤（空字符串=不过滤）
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse ListLevelActors(const FString& ClassFilter = TEXT(""));

	/**
	 * 读取单个 Actor 的核心状态（Transform / Collision / Tags）。
	 * UE5 依赖：AActor::GetActorLocation/Rotation/Scale3D() + UPrimitiveComponent + AActor::Tags
	 * @param ActorPath Actor 的完整对象路径
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse GetActorState(const FString& ActorPath);

	/**
	 * 读取 Actor 的世界包围盒。
	 * UE5 依赖：AActor::GetActorBounds() → FBoxSphereBounds
	 * @param ActorPath Actor 的完整对象路径
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse GetActorBounds(const FString& ActorPath);

	/**
	 * 读取资产基础元数据。
	 * UE5 依赖：UEditorAssetLibrary::DoesAssetExist() + FindAssetData() + UStaticMesh::GetBoundingBox()
	 * @param AssetPath 资产路径（如 /Game/Meshes/SM_Chair）
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse GetAssetMetadata(const FString& AssetPath);

	/**
	 * 读取当前未保存的脏资产列表。
	 * UE5 依赖：UEditorLoadingAndSavingUtils::GetDirtyContentPackages()
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse GetDirtyAssets();

	/**
	 * 运行地图检查。
	 * UE5 依赖：Console Command "MAP CHECK"
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse RunMapCheck();

	/**
	 * 读取指定组件的相对变换状态。
	 * UE5 依赖：Actor->GetComponents() + USceneComponent::GetRelativeTransform()
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse GetComponentState(
		const FString& ActorPath,
		const FString& ComponentName
	);

	/**
	 * 读取 Actor 当前的材质分配结果。
	 * UE5 依赖：UMeshComponent::GetNumMaterials / GetMaterial()
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Query")
	FBridgeResponse GetMaterialAssignment(const FString& ActorPath);

	// ============================================================
	// 写接口（4 个核心写操作）
	// UE5 官方能力：UEditorLevelLibrary / UAssetToolsHelpers / UBlueprintFactory
	// 全部使用 FScopedTransaction 纳入 UE5 Undo/Redo
	// ============================================================

	/**
	 * 在关卡中生成 Actor。
	 * UE5 依赖：UEditorLevelLibrary::SpawnActorFromClass() + AActor::SetActorLabel/Scale3D()
	 * Transaction：自动纳入 Undo（FScopedTransaction）
	 * @param LevelPath 目标关卡路径
	 * @param ActorClass Actor 类路径（如 /Script/Engine.StaticMeshActor）
	 * @param ActorName 显示名称
	 * @param Transform 目标 Transform
	 * @param bDryRun 仅校验参数，不实际执行
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Write")
	FBridgeResponse SpawnActor(
		const FString& LevelPath,
		const FString& ActorClass,
		const FString& ActorName,
		const FBridgeTransform& Transform,
		bool bDryRun = false
	);

	/**
	 * 修改 Actor 的 Transform。
	 * UE5 依赖：AActor::SetActorLocationAndRotation() + AActor::SetActorScale3D()
	 * Transaction：自动纳入 Undo（FScopedTransaction）
	 * @param ActorPath Actor 的完整对象路径
	 * @param Transform 目标 Transform
	 * @param bDryRun 仅校验参数，不实际执行
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Write")
	FBridgeResponse SetActorTransform(
		const FString& ActorPath,
		const FBridgeTransform& Transform,
		bool bDryRun = false
	);

	/**
	 * 批量导入外部资产。
	 * UE5 依赖：UAssetToolsHelpers::GetAssetTools()->ImportAssetTasks()
	 * Transaction：自动纳入 Undo（FScopedTransaction）
	 * @param SourceDir 源文件目录（含 .fbx / .obj / .png 等）
	 * @param DestPath 目标 Content 路径（如 /Game/Props）
	 * @param bReplaceExisting 是否覆盖已有同名资产
	 * @param bDryRun 仅列出待导入文件，不实际执行
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Write")
	FBridgeResponse ImportAssets(
		const FString& SourceDir,
		const FString& DestPath,
		bool bReplaceExisting = false,
		bool bDryRun = false
	);

	/**
	 * 创建 Blueprint 子类。
	 * UE5 依赖：UBlueprintFactory + UAssetToolsHelpers::GetAssetTools()->CreateAsset()
	 * Transaction：自动纳入 Undo（FScopedTransaction）
	 * @param ParentClass 父类路径
	 * @param PackagePath 目标包路径（如 /Game/Blueprints/BP_MyActor）
	 * @param bDryRun 仅校验参数，不实际执行
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Write")
	FBridgeResponse CreateBlueprintChild(
		const FString& ParentClass,
		const FString& PackagePath,
		bool bDryRun = false
	);

	/**
	 * 设置 Actor 根 PrimitiveComponent 的碰撞配置。
	 * UE5 依赖：UPrimitiveComponent::SetCollisionProfileName / SetCollisionEnabled / SetCanEverAffectNavigation
	 * Transaction：自动纳入 Undo（FScopedTransaction）
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Write")
	FBridgeResponse SetActorCollision(
		const FString& ActorPath,
		const FString& CollisionProfileName,
		const FString& CollisionEnabledName = TEXT("QueryAndPhysics"),
		bool bCanAffectNavigation = true,
		bool bDryRun = false
	);

	/**
	 * 为 Actor 的 MeshComponent 指定材质。
	 * UE5 依赖：LoadObject<UMaterialInterface>() + UMeshComponent::SetMaterial()
	 * Transaction：自动纳入 Undo（FScopedTransaction）
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Write")
	FBridgeResponse AssignMaterial(
		const FString& ActorPath,
		const FString& MaterialPath,
		int32 SlotIndex = 0,
		bool bDryRun = false
	);

	// ============================================================
	// 验证接口（3 个）
	// UE5 官方能力：FBox::IsInside() / UWorld::OverlapMultiByChannel()
	// ============================================================

	/**
	 * 验证 Actor 是否在指定包围盒内。
	 * UE5 依赖：AActor::GetActorBounds() + FBox::IsInside()
	 * @param ActorPath Actor 路径
	 * @param BoundsOrigin 包围盒中心 [X, Y, Z]
	 * @param BoundsExtent 包围盒半径 [X, Y, Z]
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Validate")
	FBridgeResponse ValidateActorInsideBounds(
		const FString& ActorPath,
		const FVector& BoundsOrigin,
		const FVector& BoundsExtent
	);

	/**
	 * 验证 Actor 与其他 Actor 无碰撞重叠。
	 * UE5 依赖：UWorld::OverlapMultiByChannel()
	 * @param ActorPath Actor 路径
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Validate")
	FBridgeResponse ValidateActorNonOverlap(const FString& ActorPath);

	/**
	 * 运行 UE5 自动化测试。
	 * UE5 依赖：Automation Test Framework — Console Command "Automation RunTests <Filter>"
	 * @param Filter 测试过滤表达式（如 "Project.AgentBridge.L1"）
	 * @param ReportPath 报告输出路径（可选）
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Validate")
	FBridgeResponse RunAutomationTests(
		const FString& Filter,
		const FString& ReportPath = TEXT("")
	);

	// ============================================================
	// 构建接口（1 个）
	// UE5 官方能力：UAT (BuildCookRun)
	// ============================================================

	/**
	 * 构建项目（通过 UAT 子进程）。
	 * UE5 依赖：UAT — BuildCookRun 命令
	 * @param Platform 目标平台（如 "Win64"）
	 * @param Configuration 构建配置（如 "Development"）
	 * @param bDryRun 仅输出将执行的 UAT 命令，不实际执行
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Build")
	FBridgeResponse BuildProject(
		const FString& Platform = TEXT("Win64"),
		const FString& Configuration = TEXT("Development"),
		bool bDryRun = false
	);

	// ============================================================
	// 辅助接口
	// ============================================================

	/**
	 * 保存指定的脏资产。
	 * UE5 依赖：UEditorAssetLibrary::SaveLoadedAssets()
	 * @param AssetPaths 要保存的资产路径列表（空=保存全部脏资产）
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Utility")
	FBridgeResponse SaveNamedAssets(const TArray<FString>& AssetPaths);

	/**
	 * 截取当前 Viewport 截图。
	 * UE5 依赖：TakeAutomationScreenshot() + Screenshot Comparison Tool
	 * @param ScreenshotName 截图文件名
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Utility")
	FBridgeResponse CaptureViewportScreenshot(const FString& ScreenshotName);

	/**
	 * 撤销最近的 Transaction（可通过 RC API 自动触发回滚闭环）。
	 * UE5 依赖：GEditor->UndoTransaction()
	 * @param Steps 需要撤销的步数，默认 1
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Utility")
	FBridgeResponse UndoLastTransaction(int32 Steps = 1);

	/**
	 * 获取 Subsystem 版本信息。
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|Utility")
	FString GetVersion() const { return TEXT("0.3.0"); }

	// ============================================================
	// L3 UI 工具接口（Automation Driver 执行后端）
	// UE5 官方能力：Automation Driver（IAutomationDriverModule）
	//
	// 使用优先级：L1 语义工具 > L2 编辑器服务工具 > L3 UI 工具
	// 仅当 L1 语义工具无法覆盖时使用。
	// 每次 L3 操作后，必须通过 L1 工具做独立读回验证。
	// L3 返回值与 L1 验证返回值做交叉比对——两者一致才判定 success。
	// ============================================================

	/**
	 * 在 Actor 的 Detail Panel 中点击按钮。
	 * UE5 依赖：Automation Driver (IAutomationDriver::FindElement + Click)
	 *
	 * 执行流程：选中 Actor → 在 Detail Panel 中按文本定位按钮 → 点击 → 等待 UI Idle
	 * 验证方式：调用后通过 GetActorState / GetComponentState 做 L1 独立读回
	 *
	 * @param ActorPath    目标 Actor 完整路径
	 * @param ButtonLabel  按钮显示文本（精确匹配）
	 * @param bDryRun      仅校验参数和 Driver 可用性，不实际执行 UI 操作
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|UITool")
	FBridgeResponse ClickDetailPanelButton(
		const FString& ActorPath,
		const FString& ButtonLabel,
		bool bDryRun = false
	);

	/**
	 * 在 Detail Panel 的属性输入框中输入值。
	 * UE5 依赖：Automation Driver (FindElement + CreateSequence + Type)
	 *
	 * 执行流程：选中 Actor → 定位属性行 → 点击输入框 → Ctrl+A 全选 → 输入新值 → Enter → 等待 UI Idle
	 * 验证方式：调用后通过 GetActorState 读回变更后的属性值
	 *
	 * @param ActorPath    目标 Actor 完整路径
	 * @param PropertyPath 属性路径（如 "StaticMeshComponent.StaticMesh"）
	 * @param Value        要输入的值（字符串形式）
	 * @param bDryRun      仅校验参数和 Driver 可用性
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|UITool")
	FBridgeResponse TypeInDetailPanelField(
		const FString& ActorPath,
		const FString& PropertyPath,
		const FString& Value,
		bool bDryRun = false
	);

	/**
	 * 将 Content Browser 中的资产拖拽到 Viewport 指定位置。
	 * UE5 依赖：Automation Driver (FindElement + CreateSequence + MoveToElement + Press/Release)
	 *
	 * 与 SpawnActor (L1) 的区别：拖拽走 Editor 原生 OnDropped 流程，
	 * 触发自动碰撞设置、自动命名、自动贴地等行为——SpawnActor API 不触发这些。
	 * 验证方式：调用后通过 ListLevelActors 验证 Actor 出现 + GetActorState 验证位置
	 *
	 * @param AssetPath    资产路径（如 /Game/Meshes/SM_Chair）
	 * @param DropLocation Viewport 中的世界坐标放置位置
	 * @param bDryRun      仅校验参数和 Driver 可用性
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|UITool")
	FBridgeResponse DragAssetToViewport(
		const FString& AssetPath,
		const FVector& DropLocation,
		bool bDryRun = false
	);

	/**
	 * 异步原型：启动一个 L3 UI 操作并立即返回 operation_id。
	 *
	 * 当前最小支持范围：
	 *   - click_detail_panel_button
	 *   - type_in_detail_panel_field
	 *   - drag_asset_to_viewport
	 *
	 * 设计目的：
	 *   - 验证 start_ui_operation / query_ui_operation 的异步链
	 *   - 验证 AutomationDriver::Click() 能否脱离 RC 同步链正常执行
	 *
	 * 参数说明：
	 *   OperationType  - 操作类型（当前仅支持 "click_detail_panel_button"）
	 *   ActorPath      - 目标 Actor 路径
	 *   Target         - 对 click_detail_panel_button 来说，是 ButtonLabel；
	 *                    对 type_in_detail_panel_field 来说，是 PropertyPath；
	 *                    对 drag_asset_to_viewport 来说，是 AssetPath
	 *   Value          - 对 type_in_detail_panel_field 来说，是要输入的值；
	 *                    对 drag_asset_to_viewport 来说，是 "X,Y,Z" 格式的投放坐标
	 *   TimeoutSeconds - 单次 UI 操作允许的最长耗时
	 *   bDryRun        - 仅校验参数，不真正入队执行
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|UITool")
	FBridgeResponse StartUIOperation(
		const FString& OperationType,
		const FString& ActorPath,
		const FString& Target,
		const FString& Value = TEXT(""),
		float TimeoutSeconds = 10.0f,
		bool bDryRun = false
	);

	/**
	 * 异步原型：查询 UI 操作当前状态。
	 *
	 * 返回：
	 *   顶层 FBridgeResponse 仍使用现有 success/failed 枚举，
	 *   真正的异步运行态放在 data.operation_state 中：
	 *     pending / running / success / failed
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|UITool")
	FBridgeResponse QueryUIOperation(const FString& OperationId);

	/**
	 * 检查 Automation Driver 是否可用。
	 * @return true = AutomationDriver 模块已加载且可用
	 */
	UFUNCTION(BlueprintCallable, Category = "AgentBridge|UITool")
	bool IsAutomationDriverAvailable() const;

	// ============================================================
	// L3→L1 交叉比对辅助
	// ============================================================

	/**
	 * 执行 L3 UI 操作后，自动调用 L1 接口读回并与 L3 返回值做交叉比对。
	 *
	 * 流程：
	 *   1. 执行 L3 UI 操作 → 获得 L3 返回值
	 *   2. 调用 L1 查询接口 → 获得 L1 独立读回值
	 *   3. 比对两者 → 一致=success，不一致=mismatch（含字段级差异列表）
	 *
	 * @param UIToolResponse    L3 UI 工具的返回值
	 * @param L1VerifyFunc      L1 验证的函数名（如 "GetActorState"）
	 * @param L1VerifyParams    L1 验证的参数（如 ActorPath）
	 * @return 交叉比对结果（含 final_status + mismatches）
	 */
	FBridgeUIVerification CrossVerifyUIOperation(
		const FBridgeResponse& UIToolResponse,
		const FString& L1VerifyFunc,
		const FString& L1VerifyParams
	);

private:
	// ============================================================
	// 内部辅助方法
	// ============================================================

	/** 通过完整路径查找 Actor */
	AActor* FindActorByPath(const FString& ActorPath) const;

	/** 读取 Actor 碰撞状态 → JSON */
	TSharedPtr<FJsonObject> ReadCollisionToJson(const AActor* Actor) const;

	/** 读取 Actor Tags → JSON 数组 */
	TArray<TSharedPtr<FJsonValue>> ReadTagsToJsonArray(const AActor* Actor) const;

	/** 获取当前 Editor World（带 null 检查） */
	UWorld* GetEditorWorld() const;

	/** 构造写操作的空 data 壳（created/modified/deleted/dirty_assets/validation） */
	TSharedPtr<FJsonObject> MakeEmptyWriteData() const;

	/** L3 内部：将 FUIOperationResult 转为 FBridgeResponse */
	FBridgeResponse UIOperationResultToResponse(
		const FString& OperationName,
		const struct FUIOperationResult& UIResult
	) const;

	/** 异步原型：刷新单个 UI 操作状态（查询时惰性推进） */
	void RefreshPendingUIOperation(const TSharedPtr<FPendingUIOperation>& Operation);

	/** 异步原型：将内部状态包装成统一响应 */
	FBridgeResponse BuildPendingUIOperationResponse(const TSharedPtr<FPendingUIOperation>& Operation) const;

	/** 异步原型：运行中的 UI 操作表（operation_id -> 状态） */
	TMap<FString, TSharedPtr<FPendingUIOperation>> PendingUIOperations;
};
