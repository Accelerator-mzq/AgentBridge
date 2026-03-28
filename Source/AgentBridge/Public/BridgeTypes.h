// BridgeTypes.h
// AGENT + UE5 可操作层 — Bridge 封装层核心数据类型
//
// 定义：
//   - EBridgeStatus：响应状态枚举（对应 Schemas/common/primitives.schema.json）
//   - EBridgeErrorCode：错误码枚举（对应 Tool Contract 2.4 节）
//   - FBridgeError：结构化错误对象（对应 Schemas/common/error.schema.json）
//   - FBridgeTransform：Transform 结构（对应 Schemas/common/transform.schema.json）
//   - FBridgeResponse：统一响应外壳（对应 Python bridge_core.py 的 make_response）
//   - 辅助函数：MakeResponse / MakeError / ResponseToJson
//
// 设计原则：
//   - 字段名直接映射自 UE5 C++ API 属性名（参见 field_specification_v0_1.md）
//   - 所有枚举值与 JSON Schema 定义一致
//   - FBridgeResponse 可序列化为 JSON，通过 Remote Control API 返回给外部调用者

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectWrapper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "BridgeTypes.generated.h"

// ============================================================
// 响应状态枚举
// 对应：Schemas/common/primitives.schema.json → status enum
// 对应：AGENTS.md 2.7 节
// ============================================================

UENUM(BlueprintType)
enum class EBridgeStatus : uint8
{
	Success          UMETA(DisplayName = "Success"),
	Warning          UMETA(DisplayName = "Warning"),
	Failed           UMETA(DisplayName = "Failed"),
	Mismatch         UMETA(DisplayName = "Mismatch"),
	ValidationError  UMETA(DisplayName = "Validation Error"),
};

// Status → JSON string 转换
inline FString BridgeStatusToString(EBridgeStatus Status)
{
	switch (Status)
	{
		case EBridgeStatus::Success:         return TEXT("success");
		case EBridgeStatus::Warning:         return TEXT("warning");
		case EBridgeStatus::Failed:          return TEXT("failed");
		case EBridgeStatus::Mismatch:        return TEXT("mismatch");
		case EBridgeStatus::ValidationError: return TEXT("validation_error");
		default:                             return TEXT("failed");
	}
}

// ============================================================
// 错误码枚举
// 对应：Tool Contract 2.4 节
// ============================================================

UENUM(BlueprintType)
enum class EBridgeErrorCode : uint8
{
	None                  UMETA(DisplayName = "None"),
	InvalidArgs           UMETA(DisplayName = "Invalid Args"),
	ActorNotFound         UMETA(DisplayName = "Actor Not Found"),
	AssetNotFound         UMETA(DisplayName = "Asset Not Found"),
	ClassNotFound         UMETA(DisplayName = "Class Not Found"),
	EditorNotReady        UMETA(DisplayName = "Editor Not Ready"),
	ToolExecutionFailed   UMETA(DisplayName = "Tool Execution Failed"),
	Timeout               UMETA(DisplayName = "Timeout"),
	PermissionDenied      UMETA(DisplayName = "Permission Denied"),
	// L3 UI Tool 专用错误码
	DriverNotAvailable    UMETA(DisplayName = "Driver Not Available"),
	WidgetNotFound        UMETA(DisplayName = "Widget Not Found"),
	UIOperationTimeout    UMETA(DisplayName = "UI Operation Timeout"),
};

inline FString BridgeErrorCodeToString(EBridgeErrorCode Code)
{
	switch (Code)
	{
		case EBridgeErrorCode::None:                return TEXT("");
		case EBridgeErrorCode::InvalidArgs:         return TEXT("INVALID_ARGS");
		case EBridgeErrorCode::ActorNotFound:       return TEXT("ACTOR_NOT_FOUND");
		case EBridgeErrorCode::AssetNotFound:       return TEXT("ASSET_NOT_FOUND");
		case EBridgeErrorCode::ClassNotFound:       return TEXT("CLASS_NOT_FOUND");
		case EBridgeErrorCode::EditorNotReady:      return TEXT("EDITOR_NOT_READY");
		case EBridgeErrorCode::ToolExecutionFailed: return TEXT("TOOL_EXECUTION_FAILED");
		case EBridgeErrorCode::Timeout:             return TEXT("TIMEOUT");
		case EBridgeErrorCode::PermissionDenied:    return TEXT("PERMISSION_DENIED");
		case EBridgeErrorCode::DriverNotAvailable:  return TEXT("DRIVER_NOT_AVAILABLE");
		case EBridgeErrorCode::WidgetNotFound:      return TEXT("WIDGET_NOT_FOUND");
		case EBridgeErrorCode::UIOperationTimeout:  return TEXT("UI_OPERATION_TIMEOUT");
		default:                                    return TEXT("TOOL_EXECUTION_FAILED");
	}
}

// ============================================================
// 结构化错误对象
// 对应：Schemas/common/error.schema.json
// ============================================================

USTRUCT(BlueprintType)
struct AGENTBRIDGE_API FBridgeError
{
	GENERATED_BODY()

	// 内部字段（供 C++ 逻辑使用）
	FString Code;

	FString Message;

	FString Details;

	// RC 兼容字段（小写键，直接对齐 Schema）
	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FString code;

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FString message;

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FString details;

	FBridgeError() {}

	FBridgeError(EBridgeErrorCode InCode, const FString& InMessage, const FString& InDetails = TEXT(""))
		: Code(BridgeErrorCodeToString(InCode))
		, Message(InMessage)
		, Details(InDetails)
	{
		SyncForRemote();
	}

	void SyncForRemote()
	{
		code = Code;
		message = Message;
		details = Details;
	}

	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("code"), Code);
		Obj->SetStringField(TEXT("message"), Message);
		if (!Details.IsEmpty())
		{
			Obj->SetStringField(TEXT("details"), Details);
		}
		return Obj;
	}
};

// ============================================================
// Transform 结构
// 对应：Schemas/common/transform.schema.json
// 字段名映射：
//   location       → AActor::GetActorLocation()  (FVector, cm)
//   rotation       → AActor::GetActorRotation()  (FRotator, degrees)
//   relative_scale3d → AActor::GetActorScale3D() (FVector, 倍率)
// ============================================================

USTRUCT(BlueprintType)
struct AGENTBRIDGE_API FBridgeTransform
{
	GENERATED_BODY()

	/** AActor::GetActorLocation() — 世界坐标 (cm) */
	UPROPERTY(BlueprintReadWrite, Category = "AgentBridge")
	FVector Location = FVector::ZeroVector;

	/** AActor::GetActorRotation() — 旋转角度 (degrees: Pitch/Yaw/Roll) */
	UPROPERTY(BlueprintReadWrite, Category = "AgentBridge")
	FRotator Rotation = FRotator::ZeroRotator;

	/** AActor::GetActorScale3D() — 相对缩放 */
	UPROPERTY(BlueprintReadWrite, Category = "AgentBridge")
	FVector RelativeScale3D = FVector::OneVector;

	/** 从 AActor 读取 Transform */
	static FBridgeTransform FromActor(const AActor* Actor)
	{
		FBridgeTransform T;
		if (Actor)
		{
			T.Location = Actor->GetActorLocation();
			T.Rotation = Actor->GetActorRotation();
			T.RelativeScale3D = Actor->GetActorScale3D();
		}
		return T;
	}

	/** 转为 JSON 数组格式（与 Schema 一致：[x, y, z]） */
	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());

		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShareable(new FJsonValueNumber(Location.X)));
		LocArr.Add(MakeShareable(new FJsonValueNumber(Location.Y)));
		LocArr.Add(MakeShareable(new FJsonValueNumber(Location.Z)));
		Obj->SetArrayField(TEXT("location"), LocArr);

		TArray<TSharedPtr<FJsonValue>> RotArr;
		RotArr.Add(MakeShareable(new FJsonValueNumber(Rotation.Pitch)));
		RotArr.Add(MakeShareable(new FJsonValueNumber(Rotation.Yaw)));
		RotArr.Add(MakeShareable(new FJsonValueNumber(Rotation.Roll)));
		Obj->SetArrayField(TEXT("rotation"), RotArr);

		TArray<TSharedPtr<FJsonValue>> ScaleArr;
		ScaleArr.Add(MakeShareable(new FJsonValueNumber(RelativeScale3D.X)));
		ScaleArr.Add(MakeShareable(new FJsonValueNumber(RelativeScale3D.Y)));
		ScaleArr.Add(MakeShareable(new FJsonValueNumber(RelativeScale3D.Z)));
		Obj->SetArrayField(TEXT("relative_scale3d"), ScaleArr);

		return Obj;
	}

	/** 容差比对（用于验证闭环）*/
	bool NearlyEquals(const FBridgeTransform& Other, float LocationTolerance = 0.01f, float RotationTolerance = 0.01f, float ScaleTolerance = 0.001f) const
	{
		return Location.Equals(Other.Location, LocationTolerance)
			&& Rotation.Equals(Other.Rotation, RotationTolerance)
			&& RelativeScale3D.Equals(Other.RelativeScale3D, ScaleTolerance);
	}
};

// ============================================================
// 创建/修改/删除的对象引用
// 对应：write_operation_feedback.response.schema.json
// ============================================================

USTRUCT(BlueprintType)
struct AGENTBRIDGE_API FBridgeObjectRef
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FString ActorName;

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FString ActorPath;

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FString AssetPath;

	FBridgeObjectRef() {}

	FBridgeObjectRef(const FString& InName, const FString& InPath)
		: ActorName(InName), ActorPath(InPath) {}

	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		if (!ActorName.IsEmpty())  Obj->SetStringField(TEXT("actor_name"), ActorName);
		if (!ActorPath.IsEmpty())  Obj->SetStringField(TEXT("actor_path"), ActorPath);
		if (!AssetPath.IsEmpty())  Obj->SetStringField(TEXT("asset_path"), AssetPath);
		return Obj;
	}
};

// ============================================================
// 统一响应外壳
// 对应：全部 Schema 的外壳结构 {status, summary, data, warnings, errors}
// 对应：Python bridge_core.py 的 make_response()
// ============================================================

USTRUCT(BlueprintType)
struct AGENTBRIDGE_API FBridgeResponse
{
	GENERATED_BODY()

	// 内部字段（供 C++ 逻辑与 ToJsonString 使用）
	EBridgeStatus Status = EBridgeStatus::Failed;

	FString Summary;

	/** 业务数据（JSON 对象，内容因工具而异） */
	TSharedPtr<FJsonObject> Data;

	TArray<FString> Warnings;

	TArray<FBridgeError> Errors;

	// RC 兼容字段（小写键，直接对齐 Schema）
	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FString status = TEXT("failed");

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FString summary;

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FJsonObjectWrapper data;

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	TArray<FString> warnings;

	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	TArray<FBridgeError> errors;

	/** 标记此操作是否通过 UE5 Transaction System 纳入 Undo */
	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	bool bTransaction = false;

	/** 同步 RC 可序列化镜像字段。 */
	void SyncForRemote()
	{
		if (!Data.IsValid())
		{
			Data = MakeShareable(new FJsonObject());
		}

		status = BridgeStatusToString(Status);
		summary = Summary;
		warnings = Warnings;
		errors = Errors;
		for (FBridgeError& Err : errors)
		{
			Err.SyncForRemote();
		}

		FString DataString;
		TSharedRef<TJsonWriter<>> DataWriter = TJsonWriterFactory<>::Create(&DataString);
		FJsonSerializer::Serialize(Data.ToSharedRef(), DataWriter);
		data.JsonString = DataString;
		data.JsonObject = Data;
	}

	/** 是否成功（status == success 或 warning） */
	bool IsSuccess() const
	{
		return Status == EBridgeStatus::Success || Status == EBridgeStatus::Warning;
	}

	/** 序列化为 JSON 字符串（通过 Remote Control API 返回给外部调用者） */
	FString ToJsonString() const
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());

		Root->SetStringField(TEXT("status"), BridgeStatusToString(Status));
		Root->SetStringField(TEXT("summary"), Summary);

		if (Data.IsValid())
		{
			Root->SetObjectField(TEXT("data"), Data);
		}
		else
		{
			Root->SetObjectField(TEXT("data"), MakeShareable(new FJsonObject()));
		}

		// warnings
		TArray<TSharedPtr<FJsonValue>> WarningArr;
		for (const FString& W : Warnings)
		{
			WarningArr.Add(MakeShareable(new FJsonValueString(W)));
		}
		Root->SetArrayField(TEXT("warnings"), WarningArr);

		// errors
		TArray<TSharedPtr<FJsonValue>> ErrorArr;
		for (const FBridgeError& E : Errors)
		{
			ErrorArr.Add(MakeShareable(new FJsonValueObject(E.ToJson())));
		}
		Root->SetArrayField(TEXT("errors"), ErrorArr);

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return OutputString;
	}
};

// ============================================================
// L3 UI 工具 → L1 语义工具 交叉比对结构体
// L3 执行后，拿 L3 返回值与 L1 独立读回值做比对
// 两者一致 → success，不一致 → mismatch
// ============================================================

USTRUCT(BlueprintType)
struct AGENTBRIDGE_API FBridgeUIVerification
{
	GENERATED_BODY()

	/** L3 UI 工具的返回值（执行方声称的结果） */
	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FBridgeResponse UIToolResponse;

	/** L1 语义工具的独立读回值（真实状态） */
	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	FBridgeResponse SemanticVerifyResponse;

	/** L3 与 L1 是否一致 */
	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	bool bConsistent = false;

	/** 不一致的字段列表（如 "location.X: L3=100.0, L1=100.5"） */
	UPROPERTY(BlueprintReadOnly, Category = "AgentBridge")
	TArray<FString> Mismatches;

	/** 最终判定状态：两者一致→success，不一致→mismatch，L3 或 L1 本身失败→failed */
	EBridgeStatus GetFinalStatus() const
	{
		if (!UIToolResponse.IsSuccess())
			return UIToolResponse.Status;
		if (!SemanticVerifyResponse.IsSuccess())
			return SemanticVerifyResponse.Status;
		return bConsistent ? EBridgeStatus::Success : EBridgeStatus::Mismatch;
	}

	FString GetFinalSummary() const
	{
		if (!UIToolResponse.IsSuccess())
			return FString::Printf(TEXT("L3 UI tool failed: %s"), *UIToolResponse.Summary);
		if (!SemanticVerifyResponse.IsSuccess())
			return FString::Printf(TEXT("L1 verification failed: %s"), *SemanticVerifyResponse.Summary);
		if (!bConsistent)
			return FString::Printf(TEXT("L3/L1 mismatch: %d fields differ"), Mismatches.Num());
		return TEXT("L3 UI tool result verified by L1 semantic readback");
	}

	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("final_status"), BridgeStatusToString(GetFinalStatus()));
		Obj->SetStringField(TEXT("final_summary"), GetFinalSummary());
		Obj->SetBoolField(TEXT("consistent"), bConsistent);

		TArray<TSharedPtr<FJsonValue>> MismatchArr;
		for (const FString& M : Mismatches)
		{
			MismatchArr.Add(MakeShareable(new FJsonValueString(M)));
		}
		Obj->SetArrayField(TEXT("mismatches"), MismatchArr);

		return Obj;
	}
};

// ============================================================
// 响应构造辅助函数（对应 Python bridge_core.py 的 make_response / make_error）
// ============================================================

namespace AgentBridge
{
	/** 构造成功响应 */
	inline FBridgeResponse MakeSuccess(const FString& Summary, TSharedPtr<FJsonObject> Data = nullptr)
	{
		FBridgeResponse R;
		R.Status = EBridgeStatus::Success;
		R.Summary = Summary;
		R.Data = Data ? Data : MakeShareable(new FJsonObject());
		R.SyncForRemote();
		return R;
	}

	/** 构造失败响应 */
	inline FBridgeResponse MakeFailed(const FString& Summary, EBridgeErrorCode Code, const FString& ErrorMessage)
	{
		FBridgeResponse R;
		R.Status = EBridgeStatus::Failed;
		R.Summary = Summary;
		R.Data = MakeShareable(new FJsonObject());
		R.Errors.Add(FBridgeError(Code, ErrorMessage));
		R.SyncForRemote();
		return R;
	}

	/** 构造参数校验错误响应 */
	inline FBridgeResponse MakeValidationError(const FString& FieldName, const FString& Message)
	{
		FBridgeResponse R;
		R.Status = EBridgeStatus::ValidationError;
		R.Summary = FString::Printf(TEXT("Invalid field: %s"), *FieldName);
		R.Data = MakeShareable(new FJsonObject());
		R.Errors.Add(FBridgeError(EBridgeErrorCode::InvalidArgs, Message));
		R.SyncForRemote();
		return R;
	}

	/** 构造 mismatch 响应 */
	inline FBridgeResponse MakeMismatch(const FString& Summary, TSharedPtr<FJsonObject> Data = nullptr)
	{
		FBridgeResponse R;
		R.Status = EBridgeStatus::Mismatch;
		R.Summary = Summary;
		R.Data = Data ? Data : MakeShareable(new FJsonObject());
		R.SyncForRemote();
		return R;
	}

	/** 检查 Editor 是否处于可写状态（非 PIE） */
	inline bool IsEditorReady(FBridgeResponse& OutError)
	{
		if (!GEditor)
		{
			OutError = MakeFailed(TEXT("GEditor is null"), EBridgeErrorCode::EditorNotReady, TEXT("Editor not initialized"));
			return false;
		}
		if (GEditor->PlayWorld != nullptr)
		{
			OutError = MakeFailed(TEXT("Editor is in PIE mode"), EBridgeErrorCode::EditorNotReady, TEXT("Cannot write during PIE"));
			return false;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			OutError = MakeFailed(TEXT("No editor world"), EBridgeErrorCode::EditorNotReady, TEXT("Editor world is null"));
			return false;
		}
		return true;
	}

	/** 验证 Transform 参数（对应 Python bridge_core.py 的 validate_transform） */
	inline bool ValidateTransform(const FBridgeTransform& Transform, FBridgeResponse& OutError)
	{
		// UE5 的 FVector/FRotator 总是有效的数值类型
		// 这里主要检查极端值
		if (Transform.RelativeScale3D.IsNearlyZero(KINDA_SMALL_NUMBER))
		{
			OutError = MakeValidationError(TEXT("relative_scale3d"), TEXT("Scale cannot be zero"));
			return false;
		}
		return true;
	}

	/** 验证必填字符串（对应 Python bridge_core.py 的 validate_required_string） */
	inline bool ValidateRequiredString(const FString& Value, const FString& FieldName, FBridgeResponse& OutError)
	{
		if (Value.IsEmpty() || Value.TrimStartAndEnd().IsEmpty())
		{
			OutError = MakeValidationError(FieldName, FString::Printf(TEXT("%s must be a non-empty string"), *FieldName));
			return false;
		}
		return true;
	}

	// ============================================================
	// L3 UI 工具辅助函数
	// ============================================================

	/** 构造 Automation Driver 不可用的失败响应 */
	inline FBridgeResponse MakeDriverNotAvailable()
	{
		return MakeFailed(
			TEXT("Automation Driver not available"),
			EBridgeErrorCode::DriverNotAvailable,
			TEXT("IAutomationDriverModule is not loaded. Enable the AutomationDriver plugin."));
	}

	/** 构造 Widget 未找到的失败响应 */
	inline FBridgeResponse MakeWidgetNotFound(const FString& WidgetDescription)
	{
		return MakeFailed(
			FString::Printf(TEXT("Widget not found: %s"), *WidgetDescription),
			EBridgeErrorCode::WidgetNotFound,
			FString::Printf(TEXT("Cannot locate UI element: %s"), *WidgetDescription));
	}

	/** 构造 L3→L1 交叉比对结果 */
	inline FBridgeUIVerification MakeUIVerification(
		const FBridgeResponse& UIToolResp,
		const FBridgeResponse& SemanticResp,
		bool bConsistent,
		const TArray<FString>& Mismatches = {})
	{
		FBridgeUIVerification V;
		V.UIToolResponse = UIToolResp;
		V.SemanticVerifyResponse = SemanticResp;
		V.bConsistent = bConsistent;
		V.Mismatches = Mismatches;
		return V;
	}
}

