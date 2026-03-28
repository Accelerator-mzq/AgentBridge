// AutomationDriverAdapter.h
// AGENT + UE5 可操作层 — L3 UI 工具层 Automation Driver 封装
//
// UE5 官方模块：Automation Driver（IAutomationDriverModule）
//
// 设计原则：
//   1. Agent 不直接调用 Automation Driver API——全部通过本封装层间接使用
//   2. 操作参数是语义级的（路径/名称/标签），不是坐标级（像素 x,y）
//   3. 每个操作有超时机制（WaitForUIIdle）
//   4. 操作结果通过 L1/L2 工具做独立验证——本层只负责执行
//
// 本类是内部实现，不标记 UCLASS / USTRUCT——
// 对外接口通过 UAgentBridgeSubsystem 的 L3 UFUNCTION 暴露。
//
// Automation Driver 核心能力：
//   - IAutomationDriverModule::CreateDriver() → 获取 Driver 实例
//   - IAutomationDriver::FindElement() → 通过 Locator 查找 UI 元素
//   - IDriverElement::Click() / Type() / Focus() / ScrollBy() 等
//
// Widget 定位策略：
//   优先使用 Widget 的 Tag / Id / 文本匹配，避免坐标定位。
//   UE5 的 Slate Widget 支持 SWidget::GetTag() 和文本内容匹配。

#pragma once

#include "CoreMinimal.h"
#include "BridgeTypes.h"

// 前向声明——避免头文件中直接 include Automation Driver 的头
class IAutomationDriver;
class IAsyncAutomationDriver;
class IDriverElement;

/**
 * Automation Driver 操作结果。
 * 描述单次 UI 操作的执行状态，不做业务验证——验证由 L1 工具负责。
 */
struct AGENTBRIDGE_API FUIOperationResult
{
	/** 操作是否执行成功（Driver 层面） */
	bool bExecuted = false;

	/** 如果失败，原因描述 */
	FString FailureReason;

	/** 操作耗时（秒） */
	float DurationSeconds = 0.0f;

	/** 操作后 UI 是否回到 Idle 状态 */
	bool bUIIdleAfter = false;

	/** 可选调试信息：记录本次操作匹配到的定位路径 / Tag */
	FString DebugLocatorPath;

	bool IsSuccess() const { return bExecuted && bUIIdleAfter; }
};

/**
 * L3 UI 工具层的 Automation Driver 封装。
 *
 * 将 Automation Driver 的底层 Widget 操作封装为语义级操作。
 * Agent 不直接接触本类——通过 UAgentBridgeSubsystem 的 L3 接口间接使用。
 *
 * 使用前必须确认：
 *   1. AutomationDriver Plugin 已启用
 *   2. Editor 不在 PIE 模式
 *   3. 目标 Widget 处于可见且可交互状态
 */
class AGENTBRIDGE_API FAutomationDriverAdapter
{
public:
	// ============================================================
	// 可用性检查
	// ============================================================

	/** 检查 Automation Driver 模块是否已加载且可用 */
	static bool IsAvailable();

	// ============================================================
	// 语义级 UI 操作
	// ============================================================

	/**
	 * 在 Detail Panel 中查找并点击按钮。
	 *
	 * 执行流程：
	 *   1. 确认目标 Actor 已选中（通过 GEditor->SelectActor）
	 *   2. 确认 Detail Panel 处于打开状态
	 *   3. 在 Detail Panel Widget 树中按 ButtonLabel 文本匹配查找按钮
	 *   4. 模拟点击
	 *   5. 等待 UI Idle
	 *
	 * @param ActorPath     目标 Actor 路径（用于先选中该 Actor）
	 * @param ButtonLabel   按钮显示文本（精确匹配）
	 * @param TimeoutSeconds 超时秒数
	 * @return 操作结果（不含业务验证——调用者需通过 L1 工具验证）
	 */
	static FUIOperationResult ClickDetailPanelButton(
		const FString& ActorPath,
		const FString& ButtonLabel,
		float TimeoutSeconds = 10.0f
	);

	/**
	 * 异步原型专用：在非 GameThread 上通过 AutomationDriver::Click() 点击 Detail Panel 按钮。
	 *
	 * 设计目的：
	 *   1. 避开 RC 同步链路在 GameThread 上直接等待 Driver Click 的死锁风险
	 *   2. 保留 AutomationDriver::Click() 的真实点击语义，用于验证异步链方案是否可行
	 *
	 * 执行模型：
	 *   - 调用线程：必须是非 GameThread（例如 ThreadPool 任务）
	 *   - GameThread：仅负责 Actor 选中 / Detail 面板刷新 / 定位可点击按钮 Tag
	 *   - 调用线程：负责真正执行 Driver->Wait + DriverElement->Click() 的同步等待
	 *
	 * 注意：
	 *   这是 start_ui_operation / query_ui_operation 的最小原型后端，
	 *   当前只用于验证 “异步链 + Driver Click” 是否能跑通。
	 */
	static FUIOperationResult ClickDetailPanelButtonOffGameThread(
		const FString& ActorPath,
		const FString& ButtonLabel,
		float TimeoutSeconds = 10.0f
	);

	/**
	 * 异步原型专用：在后台任务里调度 Detail Panel 文本输入。
	 *
	 * 说明：
	 *   - 当前 TypeInDetailPanelField 的实际输入链路仍依赖 Slate / Detail Panel，
	 *     因此本体依旧在 GameThread 上执行。
	 *   - 这里的价值是把“等待操作完成”从 RC 同步请求里拆出去，
	 *     让 start_ui_operation / query_ui_operation 能先把异步链跑通。
	 */
	static FUIOperationResult TypeInDetailPanelFieldAsyncPrototype(
		const FString& ActorPath,
		const FString& PropertyPath,
		const FString& Value,
		float TimeoutSeconds = 10.0f
	);

	/**
	 * 异步原型专用：在后台任务里调度拖拽资产到 Viewport。
	 *
	 * 当前拖拽逻辑仍依赖 Slate / Viewport / Content Browser 的主线程状态，
	 * 因此执行本体依旧回到 GameThread；异步壳的职责是把等待从 RC 同步链拆出去。
	 */
	static FUIOperationResult DragAssetToViewportAsyncPrototype(
		const FString& AssetPath,
		const FVector& DropLocation,
		float TimeoutSeconds = 15.0f
	);

	/**
	 * 在 Detail Panel 的属性输入框中输入值。
	 *
	 * 执行流程：
	 *   1. 确认目标 Actor 已选中
	 *   2. 在 Detail Panel 中按 PropertyPath 定位属性行
	 *   3. 点击输入框获得焦点
	 *   4. 清空旧值 → 输入新值 → 模拟 Enter 确认
	 *   5. 等待 UI Idle
	 *
	 * @param ActorPath     目标 Actor 路径
	 * @param PropertyPath  属性路径（如 "StaticMeshComponent.StaticMesh"）
	 * @param Value         要输入的值（字符串形式）
	 * @param TimeoutSeconds 超时秒数
	 * @return 操作结果
	 */
	static FUIOperationResult TypeInDetailPanelField(
		const FString& ActorPath,
		const FString& PropertyPath,
		const FString& Value,
		float TimeoutSeconds = 10.0f
	);

	/**
	 * 将 Content Browser 中的资产拖拽到 Viewport 指定位置。
	 *
	 * 执行流程：
	 *   1. 校验 AssetPath 并加载资产对象
	 *   2. 计算 Viewport 中 DropLocation 对应的投放坐标
	 *   3. 调用 LevelEditor 官方 DropObjectsAtCoordinates 放置路径
	 *   4. 等待 UI Idle + Actor 生成
	 *
	 * 为什么需要这个而不用 SpawnActor？
	 *   这里仍然走 Editor 原生 Viewport 放置行为，会触发自动碰撞设置、
	 *   自动命名、自动贴地等行为——SpawnActor API 不触发这些。
	 *
	 * @param AssetPath     资产路径（如 /Game/Meshes/SM_Chair）
	 * @param DropLocation  世界坐标放置位置
	 * @param TimeoutSeconds 超时秒数
	 * @return 操作结果
	 */
	static FUIOperationResult DragAssetToViewport(
		const FString& AssetPath,
		const FVector& DropLocation,
		float TimeoutSeconds = 15.0f
	);

	// ============================================================
	// 底层辅助（private 实现中使用）
	// ============================================================

	/**
	 * 等待 Editor UI 回到 Idle 状态。
	 * 在每次 UI 操作后调用，确保操作完成且 UI 响应恢复。
	 *
	 * @param TimeoutSeconds 最大等待秒数
	 * @return true=已 Idle，false=超时
	 */
	static bool WaitForUIIdle(float TimeoutSeconds = 5.0f);

private:
	/** 获取或创建 Automation Driver 实例 */
	static TSharedPtr<IAutomationDriver, ESPMode::ThreadSafe> GetOrCreateDriver();

	/**
	 * 选中指定 Actor 并确保 Detail Panel 打开。
	 * 这是 ClickDetailPanelButton / TypeInDetailPanelField 的共同前置步骤。
	 */
	static bool SelectActorAndOpenDetails(const FString& ActorPath);

	/**
	 * 在 Widget 树中按文本标签查找子 Widget。
	 * 支持 STextBlock 文本匹配和 SButton 内部文本匹配。
	 *
	 * @param RootWidget  搜索起点
	 * @param Label       目标文本（精确匹配）
	 * @return 找到的 Widget，nullptr 表示未找到
	 */
	static TSharedPtr<SWidget> FindWidgetByLabel(
		TSharedPtr<SWidget> RootWidget,
		const FString& Label
	);

	/**
	 * 在 Content Browser 中定位资产对应的 Widget。
	 *
	 * @param AssetPath  资产路径
	 * @return 资产缩略图 Widget，nullptr 表示未找到
	 */
	static TSharedPtr<SWidget> FindAssetInContentBrowser(const FString& AssetPath);

	/**
	 * 将世界坐标转换为 Viewport 屏幕坐标。
	 *
	 * @param WorldLocation  世界坐标
	 * @param OutScreenPos   输出的屏幕坐标
	 * @return true=在屏幕内，false=在屏幕外
	 */
	static bool WorldToScreen(const FVector& WorldLocation, FVector2D& OutScreenPos);

	/** 缓存的 Driver 实例 */
	static TSharedPtr<IAutomationDriver, ESPMode::ThreadSafe> CachedDriver;
};

