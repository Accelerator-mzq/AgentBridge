// AutomationDriverAdapter.cpp
// AGENT + UE5 可操作层 — L3 UI 工具层 Automation Driver 封装实现
//
// UE5 官方模块：Automation Driver（IAutomationDriverModule）
//
// 本文件实现语义级 UI 操作，将 Automation Driver 的底层 Widget 交互
// 封装为路径/名称/标签驱动的高层操作。

#include "AutomationDriverAdapter.h"
#include "IAutomationDriverModule.h"
#include "IAutomationDriver.h"
#include "IDriverElement.h"
#include "LocateBy.h"
#include "WaitUntil.h"

#include "Editor.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "LevelEditorViewport.h"
#include "Selection.h"
#include "EditorLevelLibrary.h"

// Slate UI
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SWindow.h"
#include "Types/ISlateMetaData.h"

// Module Manager（运行时检查 Automation Driver 模块）
#include "Modules/ModuleManager.h"

#include "Async/Async.h"
#include "Async/Future.h"
#include "Misc/App.h"

// 静态缓存
TSharedPtr<IAutomationDriver, ESPMode::ThreadSafe> FAutomationDriverAdapter::CachedDriver = nullptr;

namespace
{
	struct FDetailPropertyLocator
	{
		TArray<FString> RowTagCandidates;
		TArray<FString> LabelCandidates;
		TOptional<FString> AxisToken;
	};

	struct FPreparedAsyncDriverClickContext
	{
		bool bReady = false;
		FString FailureReason;
		FString LocatorPath;
	};

	static TSharedPtr<SLevelViewport> GetUsableLevelViewportWidget()
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			return nullptr;
		}

		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		if (TSharedPtr<SLevelViewport> ActiveViewport = LevelEditorModule.GetFirstActiveLevelViewport())
		{
			return ActiveViewport;
		}

		TSharedPtr<ILevelEditor> FirstLevelEditor = LevelEditorModule.GetFirstLevelEditor();
		if (!FirstLevelEditor.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<SLevelViewport>> Viewports = FirstLevelEditor->GetViewports();
		for (const TSharedPtr<SLevelViewport>& Viewport : Viewports)
		{
			if (Viewport.IsValid() && Viewport->GetActiveViewport() != nullptr)
			{
				return Viewport;
			}
		}

		for (const TSharedPtr<SLevelViewport>& Viewport : Viewports)
		{
			if (Viewport.IsValid())
			{
				return Viewport;
			}
		}

		return nullptr;
	}

	static bool ResolveDropAssetObject(const FString& AssetPath, UObject*& OutAssetObject, FString& OutFailureReason)
	{
		OutAssetObject = LoadObject<UObject>(nullptr, *AssetPath);
		if (!OutAssetObject)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to load asset for viewport drop: %s"), *AssetPath);
			return false;
		}

		return true;
	}

	/**
	 * 在当前线程同步等待 GameThread 执行完 Lambda。
	 *
	 * 说明：
	 *   - 这是异步原型里最关键的线程桥。
	 *   - GameThread 只负责必须在主线程做的准备/收尾工作；
	 *     真正的 AutomationDriver::Click() 同步等待留在调用线程。
	 */
	template <typename TResult, typename TCallable>
	TResult RunOnGameThreadAndWait(TCallable&& Callable)
	{
		if (IsInGameThread())
		{
			return Callable();
		}

		TPromise<TResult> Promise;
		TFuture<TResult> Future = Promise.GetFuture();

		AsyncTask(ENamedThreads::GameThread,
			[Callable = Forward<TCallable>(Callable), Promise = MoveTemp(Promise)]() mutable
			{
				Promise.SetValue(Callable());
			});

		return Future.Get();
	}

	// 详情面板里少数高价值按钮带有稳定 Tag，优先走 Tag 定位可避开本地化与控件类型差异。
	TArray<FString> GetDetailPanelTagCandidates(const FString& Label)
	{
		TArray<FString> Candidates;
		const FString Normalized = Label.TrimStartAndEnd();

		auto AddCandidate = [&Candidates](const TCHAR* Tag)
		{
			Candidates.AddUnique(Tag);
		};

		if (Normalized.Equals(TEXT("Add Component"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("AddComponent"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("添加组件"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("添加"), ESearchCase::IgnoreCase))
		{
			AddCandidate(TEXT("Actor.AddComponent"));
		}

		if (Normalized.Equals(TEXT("Edit Blueprint"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("编辑蓝图"), ESearchCase::IgnoreCase))
		{
			AddCandidate(TEXT("Actor.EditBlueprint"));
		}

		if (Normalized.Equals(TEXT("Convert to Blueprint"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("Convert To Blueprint"), ESearchCase::IgnoreCase)
			|| Normalized.Equals(TEXT("转换为蓝图"), ESearchCase::IgnoreCase))
		{
			AddCandidate(TEXT("Actor.ConvertToBlueprint"));
		}

		return Candidates;
	}

	bool IsEditableWidgetType(const TSharedRef<SWidget>& Widget)
	{
		const FName WidgetType = Widget->GetType();
		if (WidgetType == FName(TEXT("SEditableText")))
		{
			return true;
		}
		if (WidgetType == FName(TEXT("SEditableTextBox")))
		{
			return true;
		}
		if (WidgetType == FName(TEXT("SMultiLineEditableText")))
		{
			return true;
		}
		if (WidgetType == FName(TEXT("SMultiLineEditableTextBox")))
		{
			return true;
		}
		if (WidgetType == FName(TEXT("SPropertyEditorText")))
		{
			return true;
		}

		const FString TypeName = WidgetType.ToString();
		return TypeName.Contains(TEXT("NumericEntryBox"))
			|| TypeName.Contains(TEXT("NumericVectorInputBox"))
			|| TypeName.Contains(TEXT("NumericRotatorInputBox"))
			|| TypeName.Contains(TEXT("SPropertyEditorNumeric"));
	}

	bool IsTextEntryWidgetType(const TSharedRef<SWidget>& Widget)
	{
		const FName WidgetType = Widget->GetType();
		return WidgetType == FName(TEXT("SEditableText"))
			|| WidgetType == FName(TEXT("SEditableTextBox"))
			|| WidgetType == FName(TEXT("SMultiLineEditableText"))
			|| WidgetType == FName(TEXT("SMultiLineEditableTextBox"));
	}

	bool IsClickableContainerType(const TSharedRef<SWidget>& Widget)
	{
		const FName WidgetType = Widget->GetType();
		return WidgetType == FName(TEXT("SButton"))
			|| WidgetType == FName(TEXT("SComboButton"))
			|| WidgetType == FName(TEXT("SPositiveActionButton"))
			|| WidgetType == FName(TEXT("SComponentClassCombo"));
	}

	bool DoesWidgetTextMatch(const TSharedRef<SWidget>& Widget, const FString& Label)
	{
		if (Widget->GetType() != FName(TEXT("STextBlock")))
		{
			return false;
		}

		const TSharedRef<STextBlock> TextBlock = StaticCastSharedRef<STextBlock, SWidget>(Widget);
		const FString WidgetText = TextBlock->GetText().ToString().TrimStartAndEnd();
		return WidgetText.Equals(Label.TrimStartAndEnd(), ESearchCase::IgnoreCase);
	}

	TSharedPtr<SWidget> FindWidgetByLabelInVisibleWindows(
		const FString& Label,
		TArray<FString>* OutWindowTitles = nullptr);

	TSharedPtr<SWidget> FindWidgetByLabelRecursive(
		TSharedPtr<SWidget> RootWidget,
		const FString& Label)
	{
		if (!RootWidget.IsValid())
		{
			return nullptr;
		}

		const FString NormalizedLabel = Label.TrimStartAndEnd();
		const TSharedRef<SWidget> WidgetRef = RootWidget.ToSharedRef();

		// 先检查 Widget 自身文本。
		if (WidgetRef->GetType() == FName(TEXT("STextBlock")))
		{
			TSharedPtr<STextBlock> TextBlock = StaticCastSharedPtr<STextBlock>(RootWidget);
			if (TextBlock.IsValid())
			{
				const FString WidgetText = TextBlock->GetText().ToString().TrimStartAndEnd();
				if (WidgetText.Equals(NormalizedLabel, ESearchCase::IgnoreCase))
				{
					return RootWidget;
				}
			}
		}

		// 再检查显式 Tag / TagMetaData，便于直接匹配引擎内置稳定标识。
		if (!WidgetRef->GetTag().IsNone()
			&& WidgetRef->GetTag().ToString().Equals(NormalizedLabel, ESearchCase::IgnoreCase))
		{
			return RootWidget;
		}

		const TArray<TSharedRef<FTagMetaData>> AllTagMetaData = WidgetRef->GetAllMetaData<FTagMetaData>();
		for (const TSharedRef<FTagMetaData>& MetaData : AllTagMetaData)
		{
			if (MetaData->Tag.ToString().Equals(NormalizedLabel, ESearchCase::IgnoreCase))
			{
				return RootWidget;
			}
		}

		FChildren* Children = WidgetRef->GetChildren();
		if (!Children)
		{
			return nullptr;
		}

		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			TSharedPtr<SWidget> Found = FindWidgetByLabelRecursive(Child, NormalizedLabel);
			if (Found.IsValid())
			{
				// 复合按钮控件命中文本子节点时，优先返回可点击的父控件本体。
				return IsClickableContainerType(WidgetRef) ? RootWidget : Found;
			}
		}

		return nullptr;
	}

	TSharedPtr<SWidget> FindFirstEditableDescendantRecursive(TSharedPtr<SWidget> RootWidget)
	{
		if (!RootWidget.IsValid())
		{
			return nullptr;
		}

		const TSharedRef<SWidget> WidgetRef = RootWidget.ToSharedRef();
		if (IsEditableWidgetType(WidgetRef))
		{
			return RootWidget;
		}

		FChildren* Children = WidgetRef->GetChildren();
		if (!Children)
		{
			return nullptr;
		}

		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			TSharedPtr<SWidget> Found = FindFirstEditableDescendantRecursive(Child);
			if (Found.IsValid())
			{
				return Found;
			}
		}

		return nullptr;
	}

	TSharedPtr<SWidget> FindFirstTextEntryDescendantRecursive(TSharedPtr<SWidget> RootWidget)
	{
		if (!RootWidget.IsValid())
		{
			return nullptr;
		}

		const TSharedRef<SWidget> WidgetRef = RootWidget.ToSharedRef();
		if (IsTextEntryWidgetType(WidgetRef))
		{
			return RootWidget;
		}

		FChildren* Children = WidgetRef->GetChildren();
		if (!Children)
		{
			return nullptr;
		}

		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			TSharedPtr<SWidget> Found = FindFirstTextEntryDescendantRecursive(Child);
			if (Found.IsValid())
			{
				return Found;
			}
		}

		return nullptr;
	}

	bool FindEditableWidgetForAxisRecursive(
		TSharedPtr<SWidget> RootWidget,
		const FString& AxisToken,
		TArray<TSharedRef<SWidget>>& AncestorStack,
		TSharedPtr<SWidget>& OutWidget)
	{
		if (!RootWidget.IsValid())
		{
			return false;
		}

		const TSharedRef<SWidget> WidgetRef = RootWidget.ToSharedRef();
		AncestorStack.Add(WidgetRef);

		if (DoesWidgetTextMatch(WidgetRef, AxisToken))
		{
			for (int32 Index = AncestorStack.Num() - 1; Index >= 0; --Index)
			{
				if (IsEditableWidgetType(AncestorStack[Index]))
				{
					OutWidget = AncestorStack[Index];
					AncestorStack.Pop();
					return true;
				}
			}
		}

		FChildren* Children = WidgetRef->GetChildren();
		if (Children)
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				if (FindEditableWidgetForAxisRecursive(Children->GetChildAt(Index), AxisToken, AncestorStack, OutWidget))
				{
					AncestorStack.Pop();
					return true;
				}
			}
		}

		AncestorStack.Pop();
		return false;
	}

	FDetailPropertyLocator BuildDetailPropertyLocator(const FString& PropertyPath)
	{
		FDetailPropertyLocator Locator;
		const FString NormalizedPath = PropertyPath.TrimStartAndEnd();

		TArray<FString> Segments;
		NormalizedPath.ParseIntoArray(Segments, TEXT("."), true);

		const FString LeafSegment = Segments.Num() > 0 ? Segments.Last() : NormalizedPath;
		const FString ParentSegment = Segments.Num() > 1 ? Segments[Segments.Num() - 2] : FString();

		auto AddRowTag = [&Locator](const TCHAR* Tag)
		{
			Locator.RowTagCandidates.AddUnique(Tag);
		};

		auto AddLabel = [&Locator](const FString& Label)
		{
			if (!Label.IsEmpty())
			{
				Locator.LabelCandidates.AddUnique(Label);
			}
		};

		const auto IsLocationSegment = [](const FString& Segment)
		{
			return Segment.Equals(TEXT("Location"), ESearchCase::IgnoreCase)
				|| Segment.Equals(TEXT("RelativeLocation"), ESearchCase::IgnoreCase);
		};

		const auto IsRotationSegment = [](const FString& Segment)
		{
			return Segment.Equals(TEXT("Rotation"), ESearchCase::IgnoreCase)
				|| Segment.Equals(TEXT("RelativeRotation"), ESearchCase::IgnoreCase);
		};

		const auto IsScaleSegment = [](const FString& Segment)
		{
			return Segment.Equals(TEXT("Scale"), ESearchCase::IgnoreCase)
				|| Segment.Equals(TEXT("Scale3D"), ESearchCase::IgnoreCase)
				|| Segment.Equals(TEXT("RelativeScale3D"), ESearchCase::IgnoreCase);
		};

		if (IsLocationSegment(ParentSegment) || IsLocationSegment(LeafSegment))
		{
			AddRowTag(TEXT("Location"));
		}
		if (IsRotationSegment(ParentSegment) || IsRotationSegment(LeafSegment))
		{
			AddRowTag(TEXT("Rotation"));
		}
		if (IsScaleSegment(ParentSegment) || IsScaleSegment(LeafSegment))
		{
			AddRowTag(TEXT("Scale"));
		}

		if (LeafSegment.Equals(TEXT("X"), ESearchCase::IgnoreCase)
			|| LeafSegment.Equals(TEXT("Y"), ESearchCase::IgnoreCase)
			|| LeafSegment.Equals(TEXT("Z"), ESearchCase::IgnoreCase)
			|| LeafSegment.Equals(TEXT("Roll"), ESearchCase::IgnoreCase)
			|| LeafSegment.Equals(TEXT("Pitch"), ESearchCase::IgnoreCase)
			|| LeafSegment.Equals(TEXT("Yaw"), ESearchCase::IgnoreCase))
		{
			Locator.AxisToken = LeafSegment;
		}

		AddLabel(NormalizedPath);
		AddLabel(LeafSegment);
		AddLabel(ParentSegment);

		if (LeafSegment.Equals(TEXT("ActorLabel"), ESearchCase::IgnoreCase))
		{
			AddLabel(TEXT("Actor Label"));
			AddLabel(TEXT("Label"));
			AddRowTag(TEXT("ActorLabel"));
		}

		if (LeafSegment.Equals(TEXT("RelativeScale3D"), ESearchCase::IgnoreCase))
		{
			AddLabel(TEXT("Scale"));
		}
		if (LeafSegment.Equals(TEXT("RelativeLocation"), ESearchCase::IgnoreCase))
		{
			AddLabel(TEXT("Location"));
		}
		if (LeafSegment.Equals(TEXT("RelativeRotation"), ESearchCase::IgnoreCase))
		{
			AddLabel(TEXT("Rotation"));
		}

		return Locator;
	}

	TSharedPtr<SWidget> FindDetailPropertyRowInVisibleWindows(
		const FDetailPropertyLocator& Locator,
		TArray<FString>* OutWindowTitles = nullptr,
		FString* OutMatchedKey = nullptr)
	{
		for (const FString& RowTag : Locator.RowTagCandidates)
		{
			TArray<FString> WindowTitles;
			TSharedPtr<SWidget> Found = FindWidgetByLabelInVisibleWindows(RowTag, &WindowTitles);
			if (Found.IsValid())
			{
				if (OutWindowTitles)
				{
					*OutWindowTitles = MoveTemp(WindowTitles);
				}
				if (OutMatchedKey)
				{
					*OutMatchedKey = FString::Printf(TEXT("row_tag:%s"), *RowTag);
				}
				return Found;
			}
		}

		for (const FString& Label : Locator.LabelCandidates)
		{
			TArray<FString> WindowTitles;
			TSharedPtr<SWidget> Found = FindWidgetByLabelInVisibleWindows(Label, &WindowTitles);
			if (Found.IsValid())
			{
				if (OutWindowTitles)
				{
					*OutWindowTitles = MoveTemp(WindowTitles);
				}
				if (OutMatchedKey)
				{
					*OutMatchedKey = FString::Printf(TEXT("label:%s"), *Label);
				}
				return Found;
			}
		}

		return nullptr;
	}

	TSharedPtr<SWidget> FindDetailPropertyInputWidget(
		TSharedPtr<SWidget> PropertyRowWidget,
		const FDetailPropertyLocator& Locator)
	{
		if (!PropertyRowWidget.IsValid())
		{
			return nullptr;
		}

		if (Locator.AxisToken.IsSet())
		{
			TArray<TSharedRef<SWidget>> AncestorStack;
			TSharedPtr<SWidget> AxisWidget;
			if (FindEditableWidgetForAxisRecursive(PropertyRowWidget, Locator.AxisToken.GetValue(), AncestorStack, AxisWidget))
			{
				return AxisWidget;
			}
		}

		return FindFirstEditableDescendantRecursive(PropertyRowWidget);
	}

	TOptional<int32> GetAxisIndex(const TOptional<FString>& AxisToken)
	{
		if (!AxisToken.IsSet())
		{
			return TOptional<int32>();
		}

		const FString Token = AxisToken.GetValue();
		if (Token.Equals(TEXT("X"), ESearchCase::IgnoreCase)
			|| Token.Equals(TEXT("Roll"), ESearchCase::IgnoreCase))
		{
			return 0;
		}
		if (Token.Equals(TEXT("Y"), ESearchCase::IgnoreCase)
			|| Token.Equals(TEXT("Pitch"), ESearchCase::IgnoreCase))
		{
			return 1;
		}
		if (Token.Equals(TEXT("Z"), ESearchCase::IgnoreCase)
			|| Token.Equals(TEXT("Yaw"), ESearchCase::IgnoreCase))
		{
			return 2;
		}

		return TOptional<int32>();
	}

	bool IsCompositeAxisInputWidget(const TSharedRef<SWidget>& Widget)
	{
		const FString TypeName = Widget->GetType().ToString();
		return TypeName.Contains(TEXT("NumericVectorInputBox"))
			|| TypeName.Contains(TEXT("NumericRotatorInputBox"));
	}

	FVector2D GetPreferredInputClickPoint(
		const TSharedRef<SWidget>& Widget,
		const FDetailPropertyLocator& Locator)
	{
		const FGeometry WidgetGeometry = Widget->GetCachedGeometry();
		const FVector2D WidgetOrigin = WidgetGeometry.GetAbsolutePosition();
		const FVector2D WidgetSize = WidgetGeometry.GetAbsoluteSize();

		if (IsCompositeAxisInputWidget(Widget))
		{
			const TOptional<int32> AxisIndex = GetAxisIndex(Locator.AxisToken);
			if (AxisIndex.IsSet())
			{
				const float AxisSlotWidth = WidgetSize.X / 3.0f;
				return FVector2D(
					WidgetOrigin.X + AxisSlotWidth * (AxisIndex.GetValue() + 0.5f),
					WidgetOrigin.Y + WidgetSize.Y * 0.5f);
			}
		}

		return WidgetOrigin + WidgetSize * 0.5f;
	}

	void ClickWidgetAt(FSlateApplication& SlateApp, const FVector2D& ClickPoint)
	{
		const FPointerEvent MouseDownEvent(
			0, ClickPoint, ClickPoint,
			TSet<FKey>({EKeys::LeftMouseButton}),
			EKeys::LeftMouseButton, 0,
			FModifierKeysState());
		SlateApp.ProcessMouseButtonDownEvent(nullptr, MouseDownEvent);

		const FPointerEvent MouseUpEvent(
			0, ClickPoint, ClickPoint,
			TSet<FKey>(),
			EKeys::LeftMouseButton, 0,
			FModifierKeysState());
		SlateApp.ProcessMouseButtonUpEvent(MouseUpEvent);
	}

	void SendKeyPress(FSlateApplication& SlateApp, const FKey& Key)
	{
		const FKeyEvent KeyDownEvent(Key, FModifierKeysState(), 0, false, 0, 0);
		SlateApp.ProcessKeyDownEvent(KeyDownEvent);
		const FKeyEvent KeyUpEvent(Key, FModifierKeysState(), 0, false, 0, 0);
		SlateApp.ProcessKeyUpEvent(KeyUpEvent);
	}

	TOptional<FKey> GetKeyForTypedCharacter(const TCHAR Char)
	{
		switch (Char)
		{
		case TEXT('0'): return EKeys::Zero;
		case TEXT('1'): return EKeys::One;
		case TEXT('2'): return EKeys::Two;
		case TEXT('3'): return EKeys::Three;
		case TEXT('4'): return EKeys::Four;
		case TEXT('5'): return EKeys::Five;
		case TEXT('6'): return EKeys::Six;
		case TEXT('7'): return EKeys::Seven;
		case TEXT('8'): return EKeys::Eight;
		case TEXT('9'): return EKeys::Nine;
		case TEXT('.'): return EKeys::Period;
		case TEXT('-'): return EKeys::Hyphen;
		default: return TOptional<FKey>();
		}
	}

	void SendCharacterSequence(FSlateApplication& SlateApp, const FString& Text)
	{
		for (const TCHAR Char : Text)
		{
			const TOptional<FKey> KeyForChar = GetKeyForTypedCharacter(Char);
			if (KeyForChar.IsSet())
			{
				const FKeyEvent KeyDownEvent(KeyForChar.GetValue(), FModifierKeysState(), 0, false, 0, 0);
				SlateApp.ProcessKeyDownEvent(KeyDownEvent);
			}

			const FCharacterEvent CharEvent(Char, FModifierKeysState(), 0, false);
			SlateApp.ProcessKeyCharEvent(CharEvent);

			if (KeyForChar.IsSet())
			{
				const FKeyEvent KeyUpEvent(KeyForChar.GetValue(), FModifierKeysState(), 0, false, 0, 0);
				SlateApp.ProcessKeyUpEvent(KeyUpEvent);
			}
		}
	}

	bool SelectAllTextInWidget(TSharedPtr<SWidget> Widget)
	{
		if (!Widget.IsValid())
		{
			return false;
		}

		const FName WidgetType = Widget->GetType();
		if (WidgetType == FName(TEXT("SEditableText")))
		{
			StaticCastSharedPtr<SEditableText>(Widget)->SelectAllText();
			return true;
		}

		if (WidgetType == FName(TEXT("SEditableTextBox")))
		{
			StaticCastSharedPtr<SEditableTextBox>(Widget)->SelectAllText();
			return true;
		}

		return false;
	}

	bool SetTextInWidget(TSharedPtr<SWidget> Widget, const FString& Text)
	{
		if (!Widget.IsValid())
		{
			return false;
		}

		const FText TextValue = FText::FromString(Text);
		if (Widget->GetType() == FName(TEXT("SEditableText")))
		{
			StaticCastSharedPtr<SEditableText>(Widget)->SetText(TextValue);
			return true;
		}

		if (Widget->GetType() == FName(TEXT("SEditableTextBox")))
		{
			StaticCastSharedPtr<SEditableTextBox>(Widget)->SetText(TextValue);
			return true;
		}

		return false;
	}

	TSharedPtr<SWidget> FindFocusableWidgetAtPoint(FSlateApplication& SlateApp, const FVector2D& ClickPoint)
	{
		const TArray<TSharedRef<SWindow>> Windows = SlateApp.GetInteractiveTopLevelWindows();
		FWidgetPath WidgetPath = SlateApp.LocateWindowUnderMouse(ClickPoint, Windows, true);

		for (int32 Index = WidgetPath.Widgets.Num() - 1; Index >= 0; --Index)
		{
			const TSharedRef<SWidget> Candidate = WidgetPath.Widgets[Index].Widget;
			if (Candidate->SupportsKeyboardFocus())
			{
				return Candidate;
			}
		}

		return nullptr;
	}

	void AppendUniqueWindow(
		TArray<TSharedRef<SWindow>>& InOutWindows,
		const TSharedRef<SWindow>& Window)
	{
		for (const TSharedRef<SWindow>& ExistingWindow : InOutWindows)
		{
			if (&ExistingWindow.Get() == &Window.Get())
			{
				return;
			}
		}

		InOutWindows.Add(Window);
	}

	TArray<TSharedRef<SWindow>> CollectSearchableTopLevelWindows(FSlateApplication& SlateApp)
	{
		TArray<TSharedRef<SWindow>> SearchWindows;

		// 1. 优先使用当前“可见窗口”集合，命中率最高，也最贴近真实交互上下文。
		SlateApp.GetAllVisibleWindowsOrdered(SearchWindows);

		// 2. 某些会话里会出现“窗口存在但可见列表为空”的情况，此时补充模态窗口与交互窗口。
		if (const TSharedPtr<SWindow> ActiveModalWindow = SlateApp.GetActiveModalWindow())
		{
			AppendUniqueWindow(SearchWindows, ActiveModalWindow.ToSharedRef());
		}

		const TArray<TSharedRef<SWindow>> InteractiveWindows = SlateApp.GetInteractiveTopLevelWindows();
		for (const TSharedRef<SWindow>& Window : InteractiveWindows)
		{
			AppendUniqueWindow(SearchWindows, Window);
		}

		// 3. 最后一层兜底：即使窗口当前不在“可见窗口”集合里，也允许扫描顶层窗口，
		//    避免后台/最小化/布局恢复中的编辑器窗口被完全漏掉。
		const TArray<TSharedRef<SWindow>> TopLevelWindows = SlateApp.GetTopLevelWindows();
		for (const TSharedRef<SWindow>& Window : TopLevelWindows)
		{
			AppendUniqueWindow(SearchWindows, Window);
		}

		return SearchWindows;
	}

	TSharedPtr<SWidget> FindWidgetByLabelInVisibleWindows(
		const FString& Label,
		TArray<FString>* OutWindowTitles)
	{
		FSlateApplication& SlateApp = FSlateApplication::Get();
		const TArray<TSharedRef<SWindow>> VisibleWindows = CollectSearchableTopLevelWindows(SlateApp);

		for (const TSharedRef<SWindow>& Window : VisibleWindows)
		{
			if (OutWindowTitles)
			{
				const FString WindowTitle = Window->GetTitle().ToString();
				OutWindowTitles->AddUnique(WindowTitle.IsEmpty() ? TEXT("<UntitledWindow>") : WindowTitle);
			}

			TSharedPtr<SWidget> Found = FindWidgetByLabelRecursive(Window, Label);
			if (Found.IsValid())
			{
				return Found;
			}
		}

		return nullptr;
	}

}

// ============================================================
// 可用性检查
// ============================================================

bool FAutomationDriverAdapter::IsAvailable()
{
	// NullRHI / headless 会话没有可交互的 Slate 视口。
	// 这种情况下必须把 L3 视为不可用，让上层测试走 graceful skip，
	// 否则后续投影/拖放会把引擎带到零尺寸视口路径。
	if (!FApp::CanEverRender())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[AgentBridge L3] AutomationDriver unavailable in current session: Current session cannot render UI (NullRHI/headless)"));
		return false;
	}

	if (!FModuleManager::Get().ModuleExists(TEXT("AutomationDriver")))
	{
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] AutomationDriver module does not exist"));
		return false;
	}

	IAutomationDriverModule* Module = FModuleManager::LoadModulePtr<IAutomationDriverModule>(TEXT("AutomationDriver"));
	if (!Module)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] Failed to load AutomationDriver module"));
		return false;
	}

	if (!Module->IsEnabled())
	{
		UE_LOG(LogTemp, Log, TEXT("[AgentBridge L3] Enabling AutomationDriver module"));
		Module->Enable();
	}

	return Module->IsEnabled();
}

// ============================================================
// Driver 实例管理
// ============================================================

TSharedPtr<IAutomationDriver, ESPMode::ThreadSafe> FAutomationDriverAdapter::GetOrCreateDriver()
{
	if (!IsAvailable())
	{
		return nullptr;
	}

	if (!CachedDriver.IsValid())
	{
		IAutomationDriverModule& Module = IAutomationDriverModule::Get();
		UE_LOG(LogTemp, Log, TEXT("[AgentBridge L3] Creating Automation Driver instance"));
		CachedDriver = Module.CreateDriver();
	}

	return CachedDriver;
}

// ============================================================
// 共用前置：选中 Actor + 打开 Detail Panel
// ============================================================

bool FAutomationDriverAdapter::SelectActorAndOpenDetails(const FString& ActorPath)
{
	if (!GEditor) return false;

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return false;

	// 查找 Actor
	AActor* TargetActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetPathName() == ActorPath)
		{
			TargetActor = *It;
			break;
		}
	}

	if (!TargetActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] Actor not found: %s"), *ActorPath);
		return false;
	}

	// 选中 Actor（触发 Detail Panel 内容刷新）
	GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
	GEditor->SelectActor(TargetActor, /*bInSelected=*/true, /*bNotify=*/true, /*bSelectEvenIfHidden=*/true);
	GEditor->NoteSelectionChange();

	// 聚合自动化顺序下，Details 面板不一定已经打开。
	// 显式唤起一次 Selection Details，可显著提高后续属性行定位稳定性。
	if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		LevelEditorModule.SummonSelectionDetails();
	}

	// 给详情面板一点刷新时间，避免后续定位读到旧树。
	FSlateApplication::Get().Tick();
	FPlatformProcess::Sleep(0.15f);
	FSlateApplication::Get().Tick();

	UE_LOG(LogTemp, Log, TEXT("[AgentBridge L3] Selected actor: %s"), *TargetActor->GetActorNameOrLabel());
	return true;
}

// ============================================================
// 操作 1: ClickDetailPanelButton
// ============================================================

FUIOperationResult FAutomationDriverAdapter::ClickDetailPanelButton(
	const FString& ActorPath,
	const FString& ButtonLabel,
	float TimeoutSeconds)
{
	FUIOperationResult Result;
	double StartTime = FPlatformTime::Seconds();

	if (!IsAvailable())
	{
		Result.FailureReason = TEXT("Automation Driver not available");
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] TypeInDetailPanelField failed: %s"), *Result.FailureReason);
		return Result;
	}

	if (!SelectActorAndOpenDetails(ActorPath))
	{
		Result.FailureReason = FString::Printf(TEXT("Failed to select actor: %s"), *ActorPath);
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] TypeInDetailPanelField failed: %s"), *Result.FailureReason);
		return Result;
	}

	TArray<FString> VisibleWindowTitles;
	TSharedPtr<SWidget> ButtonWidget = nullptr;

	// 先用稳定 Tag 做定位，真正点击仍走 Slate 鼠标事件，避免 AutomationDriver::Click() 阻塞 RC 调用。
	const TArray<FString> TagCandidates = GetDetailPanelTagCandidates(ButtonLabel);
	for (const FString& Tag : TagCandidates)
	{
		TArray<FString> TagWindowTitles;
		ButtonWidget = FindWidgetByLabelInVisibleWindows(Tag, &TagWindowTitles);
		if (ButtonWidget.IsValid())
		{
			UE_LOG(LogTemp, Log, TEXT("[AgentBridge L3] Found detail button '%s' via tag '%s'"), *ButtonLabel, *Tag);
			VisibleWindowTitles = MoveTemp(TagWindowTitles);
			break;
		}
	}

	// 通用兜底：跨所有可见顶层窗口做文本/Tag 搜索，不再依赖“当前活跃窗口”。
	if (!ButtonWidget.IsValid())
	{
		ButtonWidget = FindWidgetByLabelInVisibleWindows(ButtonLabel, &VisibleWindowTitles);
	}

	if (!ButtonWidget.IsValid())
	{
		Result.FailureReason = FString::Printf(
			TEXT("Button not found: '%s' (searched windows: %s)"),
			*ButtonLabel,
			VisibleWindowTitles.Num() > 0 ? *FString::Join(VisibleWindowTitles, TEXT(", ")) : TEXT("<none>"));
		return Result;
	}

	// 计算按钮中心坐标并模拟点击
	FSlateApplication& SlateApp = FSlateApplication::Get();
	FGeometry ButtonGeometry = ButtonWidget->GetCachedGeometry();
	FVector2D ButtonCenter = ButtonGeometry.GetAbsolutePosition()
		+ ButtonGeometry.GetAbsoluteSize() * 0.5f;

	// Mouse Down
	FPointerEvent MouseDownEvent(
		0, ButtonCenter, ButtonCenter,
		TSet<FKey>({EKeys::LeftMouseButton}),
		EKeys::LeftMouseButton, 0,
		FModifierKeysState()
	);
	SlateApp.ProcessMouseButtonDownEvent(nullptr, MouseDownEvent);

	// Mouse Up
	FPointerEvent MouseUpEvent(
		0, ButtonCenter, ButtonCenter,
		TSet<FKey>(),
		EKeys::LeftMouseButton, 0,
		FModifierKeysState()
	);
	SlateApp.ProcessMouseButtonUpEvent(MouseUpEvent);

	UE_LOG(LogTemp, Log, TEXT("[AgentBridge L3] Clicked button '%s' at (%.0f, %.0f)"),
		*ButtonLabel, ButtonCenter.X, ButtonCenter.Y);

	Result.bUIIdleAfter = WaitForUIIdle(TimeoutSeconds);
	Result.bExecuted = true;
	Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

FUIOperationResult FAutomationDriverAdapter::ClickDetailPanelButtonOffGameThread(
	const FString& ActorPath,
	const FString& ButtonLabel,
	float TimeoutSeconds)
{
	FUIOperationResult Result;
	const double StartTime = FPlatformTime::Seconds();

	if (!IsAvailable())
	{
		Result.FailureReason = TEXT("Automation Driver not available");
		return Result;
	}

	if (IsInGameThread())
	{
		Result.FailureReason = TEXT("Async prototype requires a non-GameThread caller");
		return Result;
	}

	// GameThread 只做必须的 UI 准备：
	//   1. 选中 Actor 并刷新 Detail Panel
	//   2. 用现有稳定 Tag 策略确认按钮确实存在
	//   3. 产出 Driver 可消费的 path locator
	const FPreparedAsyncDriverClickContext PreparedContext =
		RunOnGameThreadAndWait<FPreparedAsyncDriverClickContext>(
			[ActorPath, ButtonLabel]()
			{
				FPreparedAsyncDriverClickContext Context;

				if (!FAutomationDriverAdapter::IsAvailable())
				{
					Context.FailureReason = TEXT("Automation Driver not available");
					return Context;
				}

				if (!SelectActorAndOpenDetails(ActorPath))
				{
					Context.FailureReason = FString::Printf(TEXT("Failed to select actor: %s"), *ActorPath);
					return Context;
				}

				const TArray<FString> TagCandidates = GetDetailPanelTagCandidates(ButtonLabel);
				for (const FString& Tag : TagCandidates)
				{
					TArray<FString> VisibleWindowTitles;
					if (FindWidgetByLabelInVisibleWindows(Tag, &VisibleWindowTitles).IsValid())
					{
						Context.bReady = true;
						// By::Path("TagName") 本身就会从所有可见顶层窗口向下搜索。
						// 这里不要写成 "//TagName"：UE5.5.4 的路径解析器对前导双斜杠存在数组越界断言。
						Context.LocatorPath = Tag;
						return Context;
					}
				}

				Context.FailureReason = FString::Printf(
					TEXT("Async driver prototype currently requires a stable detail button tag: '%s'"),
					*ButtonLabel);
				return Context;
			});

	if (!PreparedContext.bReady)
	{
		Result.FailureReason = PreparedContext.FailureReason;
		Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
		return Result;
	}

	Result.DebugLocatorPath = PreparedContext.LocatorPath;

	// 真正的 Driver Click 同步等待留在当前后台线程，避免再次卡住 RC 同步调用的主线程栈。
	IAutomationDriverModule& Module = IAutomationDriverModule::Get();
	TSharedRef<IAutomationDriver, ESPMode::ThreadSafe> Driver = Module.CreateDriver();
	const TSharedRef<IElementLocator, ESPMode::ThreadSafe> Locator = By::Path(PreparedContext.LocatorPath);

	const bool bInteractable = Driver->Wait(
		Until::ElementIsInteractable(
			Locator,
			FWaitTimeout::InSeconds(TimeoutSeconds)));

	if (!bInteractable)
	{
		Result.FailureReason = FString::Printf(
			TEXT("Timed out waiting for interactable driver element: %s"),
			*PreparedContext.LocatorPath);
		Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
		return Result;
	}

	const TSharedRef<IDriverElement, ESPMode::ThreadSafe> DriverElement = Driver->FindElement(Locator);
	const bool bClicked = DriverElement->Click();
	if (!bClicked)
	{
		Result.FailureReason = FString::Printf(
			TEXT("AutomationDriver::Click() returned false for locator: %s"),
			*PreparedContext.LocatorPath);
		Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
		return Result;
	}

	Result.bExecuted = true;
	Result.bUIIdleAfter = RunOnGameThreadAndWait<bool>(
		[TimeoutSeconds]()
		{
			return FAutomationDriverAdapter::WaitForUIIdle(TimeoutSeconds);
		});

	if (!Result.bUIIdleAfter)
	{
		Result.FailureReason = TEXT("UI did not return to idle after async driver click");
	}

	Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

FUIOperationResult FAutomationDriverAdapter::TypeInDetailPanelFieldAsyncPrototype(
	const FString& ActorPath,
	const FString& PropertyPath,
	const FString& Value,
	float TimeoutSeconds)
{
	const double StartTime = FPlatformTime::Seconds();

	if (IsInGameThread())
	{
		FUIOperationResult Result;
		Result.FailureReason = TEXT("Async prototype requires a non-GameThread caller");
		return Result;
	}

	// 这里故意不把整段文本输入逻辑搬到后台线程。
	// 当前最小原型只验证“RC 请求立即返回 + 后台任务等待 GT UI 操作完成”这条链是否成立。
	FUIOperationResult Result = RunOnGameThreadAndWait<FUIOperationResult>(
		[ActorPath, PropertyPath, Value, TimeoutSeconds]()
		{
			return FAutomationDriverAdapter::TypeInDetailPanelField(
				ActorPath,
				PropertyPath,
				Value,
				TimeoutSeconds);
		});

	Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

FUIOperationResult FAutomationDriverAdapter::DragAssetToViewportAsyncPrototype(
	const FString& AssetPath,
	const FVector& DropLocation,
	float TimeoutSeconds)
{
	const double StartTime = FPlatformTime::Seconds();

	if (IsInGameThread())
	{
		FUIOperationResult Result;
		Result.FailureReason = TEXT("Async prototype requires a non-GameThread caller");
		return Result;
	}

	FUIOperationResult Result = RunOnGameThreadAndWait<FUIOperationResult>(
		[AssetPath, DropLocation, TimeoutSeconds]()
		{
			return FAutomationDriverAdapter::DragAssetToViewport(
				AssetPath,
				DropLocation,
				TimeoutSeconds);
		});

	Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

// ============================================================
// 操作 2: TypeInDetailPanelField
// ============================================================

FUIOperationResult FAutomationDriverAdapter::TypeInDetailPanelField(
	const FString& ActorPath,
	const FString& PropertyPath,
	const FString& Value,
	float TimeoutSeconds)
{
	FUIOperationResult Result;
	double StartTime = FPlatformTime::Seconds();

	if (!IsAvailable())
	{
		Result.FailureReason = TEXT("Automation Driver not available");
		return Result;
	}

	if (!SelectActorAndOpenDetails(ActorPath))
	{
		Result.FailureReason = FString::Printf(TEXT("Failed to select actor: %s"), *ActorPath);
		return Result;
	}

	// 按 PropertyPath 最后一段作为标签查找
	const FDetailPropertyLocator Locator = BuildDetailPropertyLocator(PropertyPath);
	TArray<FString> VisibleWindowTitles;
	FString MatchedLocator;
	TSharedPtr<SWidget> PropertyRowWidget = FindDetailPropertyRowInVisibleWindows(Locator, &VisibleWindowTitles, &MatchedLocator);

	if (!PropertyRowWidget.IsValid())
	{
		Result.FailureReason = FString::Printf(
			TEXT("Property not found: '%s' (searched windows: %s)"),
			*PropertyPath,
			VisibleWindowTitles.Num() > 0 ? *FString::Join(VisibleWindowTitles, TEXT(", ")) : TEXT("<none>"));
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] TypeInDetailPanelField failed: %s"), *Result.FailureReason);
		return Result;
	}

	TSharedPtr<SWidget> PropertyInputWidget = FindDetailPropertyInputWidget(PropertyRowWidget, Locator);
	if (!PropertyInputWidget.IsValid())
	{
		Result.FailureReason = FString::Printf(
			TEXT("Editable input not found for property: '%s' (locator: %s)"),
			*PropertyPath,
			MatchedLocator.IsEmpty() ? TEXT("<unknown>") : *MatchedLocator);
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] TypeInDetailPanelField failed: %s"), *Result.FailureReason);
		return Result;
	}

	// 模拟键盘输入：点击获焦 → 清空旧值 → 输入新值 → Enter
	FSlateApplication& SlateApp = FSlateApplication::Get();
	const TSharedRef<SWidget> InputWidgetRef = PropertyInputWidget.ToSharedRef();
	const FVector2D ClickPoint = GetPreferredInputClickPoint(InputWidgetRef, Locator);
	ClickWidgetAt(SlateApp, ClickPoint);

	// 数值输入框外层常常是 SSpinBox / NumericEntry 容器。
	// 如果直接把焦点给外层，Delete 会被编辑器解释为全局“删除 Actor”命令。
	// 这里优先钻到真正的文本输入子控件，再退回到点击命中的 focusable Widget。
	TSharedPtr<SWidget> FocusTargetWidget = FindFirstTextEntryDescendantRecursive(PropertyInputWidget);
	if (!FocusTargetWidget.IsValid())
	{
		TSharedPtr<SWidget> WidgetUnderClick = FindFocusableWidgetAtPoint(SlateApp, ClickPoint);
		if (WidgetUnderClick.IsValid())
		{
			TSharedPtr<SWidget> NestedTextEntry = FindFirstTextEntryDescendantRecursive(WidgetUnderClick);
			FocusTargetWidget = NestedTextEntry.IsValid() ? NestedTextEntry : WidgetUnderClick;
		}
	}
	if (!FocusTargetWidget.IsValid())
	{
		FocusTargetWidget = InputWidgetRef;
	}
	if (FocusTargetWidget.IsValid() && FocusTargetWidget->SupportsKeyboardFocus())
	{
		SlateApp.SetKeyboardFocus(FocusTargetWidget.ToSharedRef(), EFocusCause::SetDirectly);
	}
	FSlateApplication::Get().Tick();
	FPlatformProcess::Sleep(0.05f);
	FSlateApplication::Get().Tick();

	const bool bDirectTextEntryFocus = FocusTargetWidget.IsValid()
		&& IsTextEntryWidgetType(FocusTargetWidget.ToSharedRef());

	// 只有在真正的文本输入控件拿到焦点后，才允许做“选中全部”的本地文本操作。
	// 这里直接调用 Slate 文本控件自己的 SelectAllText，避免再走会触发全局快捷键的 Ctrl+A/Delete。
	bool bSelectAllApplied = false;
	if (bDirectTextEntryFocus)
	{
		bSelectAllApplied = SelectAllTextInWidget(FocusTargetWidget);
	}
	bool bDirectTextSetApplied = false;
	if (bDirectTextEntryFocus)
	{
		bDirectTextSetApplied = SetTextInWidget(FocusTargetWidget, Value);
	}
	if (!bDirectTextSetApplied)
	{
		SendCharacterSequence(SlateApp, Value);
	}

	// Enter 确认
	SendKeyPress(SlateApp, EKeys::Enter);

	UE_LOG(LogTemp, Log, TEXT("[AgentBridge L3] Typed '%s' into '%s' via %s (widget type: %s, focus type: %s, direct_text_focus=%s, select_all_applied=%s, direct_text_set=%s, click=(%.1f, %.1f))"),
		*Value,
		*PropertyPath,
		MatchedLocator.IsEmpty() ? TEXT("<unknown>") : *MatchedLocator,
		*PropertyInputWidget->GetType().ToString(),
		FocusTargetWidget.IsValid() ? *FocusTargetWidget->GetType().ToString() : TEXT("<none>"),
		bDirectTextEntryFocus ? TEXT("true") : TEXT("false"),
		bSelectAllApplied ? TEXT("true") : TEXT("false"),
		bDirectTextSetApplied ? TEXT("true") : TEXT("false"),
		ClickPoint.X,
		ClickPoint.Y);

	Result.bUIIdleAfter = WaitForUIIdle(TimeoutSeconds);
	Result.bExecuted = true;
	Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

// ============================================================
// 操作 3: DragAssetToViewport
// ============================================================

FUIOperationResult FAutomationDriverAdapter::DragAssetToViewport(
	const FString& AssetPath,
	const FVector& DropLocation,
	float TimeoutSeconds)
{
	FUIOperationResult Result;
	double StartTime = FPlatformTime::Seconds();
	auto FailAndReturn = [&Result, StartTime](const FString& FailureReason) -> FUIOperationResult
	{
		Result.FailureReason = FailureReason;
		Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
		UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] DragAssetToViewport failed: %s"), *FailureReason);
		return Result;
	};

	// 这里不再依赖“鼠标按下/移动/抬起”去猜测 Slate 是否真的进入拖放会话。
	// 根因上，Viewport 放置链最终认的是官方 DropObjectsAtCoordinates，
	// 而不是一串看起来像拖拽的裸鼠标事件。

	// 1. 校验资产对象
	UObject* AssetObject = nullptr;
	if (!ResolveDropAssetObject(AssetPath, AssetObject, Result.FailureReason))
	{
		return FailAndReturn(Result.FailureReason);
	}

	// 2. 获取一个可用的 Level Viewport。
	// 在自动化命令行会话里，“活跃 Viewport”有时为空，但实际仍存在可渲染的 Level Viewport。
	TSharedPtr<SLevelViewport> LevelViewportWidget = GetUsableLevelViewportWidget();
	if (!LevelViewportWidget.IsValid())
	{
		return FailAndReturn(TEXT("Usable LevelViewport widget not available"));
	}
	LevelViewportWidget->SetKeyboardFocusToThisViewport();

	// 3. 世界坐标 → Viewport 像素坐标
	FVector2D ViewportDropPos;
	bool bUsedProjection = WorldToScreen(DropLocation, ViewportDropPos);
	if (!bUsedProjection)
	{
		FViewport* ActiveViewport = LevelViewportWidget->GetActiveViewport();
		if (!ActiveViewport)
		{
			return FailAndReturn(TEXT("Drop location is outside viewport and active viewport is unavailable"));
		}

		const FIntPoint ViewportSize = ActiveViewport->GetSizeXY();
		if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			return FailAndReturn(TEXT("Drop location is outside viewport and viewport size is invalid"));
		}

		// 对可渲染 Editor 会话，如果请求世界点当前不可投影，
		// 则回退到视口中心继续走官方拖放链，避免因为相机朝向变化直接把整条 L3 链打断。
		// 旧日志已经验证过这条护栏可以稳定恢复 AllTests。
		ViewportDropPos = FVector2D(
			static_cast<float>(ViewportSize.X) * 0.5f,
			static_cast<float>(ViewportSize.Y) * 0.5f);

		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[AgentBridge L3] Requested drop location is outside viewport; fallback to viewport center (%.1f, %.1f) before snapping to requested world location"),
			ViewportDropPos.X,
			ViewportDropPos.Y);
	}

	// 4. 直接走官方 Viewport 放置路径，仍然保留编辑器拖放的放置行为。
	FLevelEditorViewportClient& LevelViewportClient =
		const_cast<FLevelEditorViewportClient&>(LevelViewportWidget->GetLevelViewportClient());

	TArray<UObject*> DroppedObjects;
	DroppedObjects.Add(AssetObject);

	TArray<AActor*> NewActors;
	const bool bDropped = LevelViewportClient.DropObjectsAtCoordinates(
		FMath::RoundToInt(ViewportDropPos.X),
		FMath::RoundToInt(ViewportDropPos.Y),
		DroppedObjects,
		NewActors,
		false,
		false,
		true,
		nullptr);

	if (!bDropped)
	{
		return FailAndReturn(FString::Printf(
			TEXT("Viewport drop did not create actors for asset: %s"),
			*AssetPath));
	}

	if (!bUsedProjection && NewActors.Num() > 0)
	{
		AActor* AnchorActor = nullptr;
		for (AActor* NewActor : NewActors)
		{
			if (IsValid(NewActor))
			{
				AnchorActor = NewActor;
				break;
			}
		}

		if (IsValid(AnchorActor))
		{
			const FVector TranslationDelta = DropLocation - AnchorActor->GetActorLocation();
			for (AActor* NewActor : NewActors)
			{
				if (!IsValid(NewActor))
				{
					continue;
				}

				NewActor->SetActorLocation(
					NewActor->GetActorLocation() + TranslationDelta,
					false,
					nullptr,
					ETeleportType::TeleportPhysics);
				NewActor->PostEditMove(true);
				NewActor->MarkPackageDirty();
			}

			UE_LOG(
				LogTemp,
				Log,
				TEXT("[AgentBridge L3] Snapped %d dropped actor(s) back to requested world location delta=(%.1f, %.1f, %.1f)"),
				NewActors.Num(),
				TranslationDelta.X,
				TranslationDelta.Y,
				TranslationDelta.Z);
		}
	}

	Result.DebugLocatorPath = AssetPath;
	Result.bExecuted = true;
	Result.bUIIdleAfter = WaitForUIIdle(TimeoutSeconds);

	UE_LOG(LogTemp, Log,
		TEXT("[AgentBridge L3] Viewport-dropped '%s' to world (%.1f, %.1f, %.1f), new actors=%d, used_projection=%s"),
		*AssetPath,
		DropLocation.X,
		DropLocation.Y,
		DropLocation.Z,
		NewActors.Num(),
		bUsedProjection ? TEXT("true") : TEXT("false"));

	Result.DurationSeconds = FPlatformTime::Seconds() - StartTime;
	return Result;
}

// ============================================================
// WaitForUIIdle
// ============================================================

bool FAutomationDriverAdapter::WaitForUIIdle(float TimeoutSeconds)
{
	double StartTime = FPlatformTime::Seconds();
	double EndTime = StartTime + TimeoutSeconds;

	while (FPlatformTime::Seconds() < EndTime)
	{
		FSlateApplication& SlateApp = FSlateApplication::Get();
		SlateApp.Tick();

		if (!SlateApp.IsDragDropping())
		{
			FPlatformProcess::Sleep(0.1f);
			SlateApp.Tick();
			if (!SlateApp.IsDragDropping())
			{
				return true;
			}
		}

		FPlatformProcess::Sleep(0.05f);
	}

	UE_LOG(LogTemp, Warning, TEXT("[AgentBridge L3] UI idle timeout after %.1fs"), TimeoutSeconds);
	return false;
}

// ============================================================
// Widget 查找辅助
// ============================================================

TSharedPtr<SWidget> FAutomationDriverAdapter::FindWidgetByLabel(
	TSharedPtr<SWidget> RootWidget,
	const FString& Label)
{
	return FindWidgetByLabelRecursive(RootWidget, Label);
}

TSharedPtr<SWidget> FAutomationDriverAdapter::FindAssetInContentBrowser(const FString& AssetPath)
{
	if (!GEditor) return nullptr;

	// 导航 Content Browser 到目标资产
	FString Cmd = FString::Printf(TEXT("ContentBrowser.FocusAsset %s"), *AssetPath);
	GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Cmd);

	FPlatformProcess::Sleep(0.5f);
	FSlateApplication::Get().Tick();

	UE_LOG(LogTemp, Log, TEXT("[AgentBridge L3] Content Browser navigated to: %s"), *AssetPath);

	// 返回当前焦点 Widget 作为拖拽起点
	return FSlateApplication::Get().GetKeyboardFocusedWidget();
}

// ============================================================
// 世界坐标 → 屏幕坐标
// ============================================================

bool FAutomationDriverAdapter::WorldToScreen(const FVector& WorldLocation, FVector2D& OutScreenPos)
{
	if (!GEditor) return false;

	TSharedPtr<SLevelViewport> LevelViewportWidget = GetUsableLevelViewportWidget();
	if (!LevelViewportWidget.IsValid())
	{
		return false;
	}

	FViewport* ActiveViewport = LevelViewportWidget->GetActiveViewport();
	if (!ActiveViewport)
	{
		return false;
	}

	const FIntPoint ViewportSize = ActiveViewport->GetSizeXY();
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[AgentBridge L3] WorldToScreen aborted: viewport size is invalid (%d x %d)"),
			ViewportSize.X,
			ViewportSize.Y);
		return false;
	}

	FLevelEditorViewportClient* ViewportClient = &LevelViewportWidget->GetLevelViewportClient();
	if (!ViewportClient)
	{
		return false;
	}

	// 构建场景视图
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		ActiveViewport,
		ViewportClient->GetScene(),
		ViewportClient->EngineShowFlags
	));

	FSceneView* View = ViewportClient->CalcSceneView(&ViewFamily);
	if (!View) return false;

	if (View->UnconstrainedViewRect.Width() <= 0 || View->UnconstrainedViewRect.Height() <= 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[AgentBridge L3] WorldToScreen aborted: view rect is invalid (%d x %d)"),
			View->UnconstrainedViewRect.Width(),
			View->UnconstrainedViewRect.Height());
		return false;
	}

	// 投影
	return FSceneView::ProjectWorldToScreen(
		WorldLocation,
		View->UnconstrainedViewRect,
		View->ViewMatrices.GetViewProjectionMatrix(),
		OutScreenPos
	);
}

