// L1_WriteTests.cpp
// L1 写接口自动化测试（4 个）。

#include "Misc/AutomationTest.h"
#include "AgentBridgeSubsystem.h"
#include "BridgeTypes.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/App.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/DateTime.h"

namespace
{
// 获取 Subsystem。
static UAgentBridgeSubsystem* GetSubsystem_Write(FAutomationTestBase& Test)
{
    UAgentBridgeSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>() : nullptr;
    if (!Subsystem)
    {
        Test.AddError(TEXT("AgentBridgeSubsystem 不可用，请确认 AgentBridge 插件已启用。"));
    }
    return Subsystem;
}

static FBridgeTransform MakeTransform(float X, float Y, float Z, float Yaw = 0.0f, float Scale = 1.0f)
{
    FBridgeTransform T;
    T.Location = FVector(X, Y, Z);
    T.Rotation = FRotator(0.0f, Yaw, 0.0f);
    T.RelativeScale3D = FVector(Scale, Scale, Scale);
    return T;
}

static bool HasErrorCode(const FBridgeResponse& Response, const TCHAR* Code)
{
    return Response.Errors.Num() > 0 && Response.Errors[0].Code == Code;
}

static bool ContainsActorName(UAgentBridgeSubsystem* Subsystem, const FString& ActorName)
{
    const FBridgeResponse ListResp = Subsystem->ListLevelActors();
    if (!ListResp.IsSuccess() || !ListResp.Data.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
    if (!ListResp.Data->TryGetArrayField(TEXT("actors"), Actors) || !Actors)
    {
        return false;
    }

    for (const TSharedPtr<FJsonValue>& Item : *Actors)
    {
        const TSharedPtr<FJsonObject> Obj = Item.IsValid() ? Item->AsObject() : nullptr;
        if (Obj.IsValid() && Obj->HasField(TEXT("actor_name")) && Obj->GetStringField(TEXT("actor_name")) == ActorName)
        {
            return true;
        }
    }

    return false;
}

static FString GetCreatedActorPath(const FBridgeResponse& SpawnResp)
{
    if (!SpawnResp.Data.IsValid()) return FString();

    const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
    if (!SpawnResp.Data->TryGetArrayField(TEXT("created_objects"), Created) || !Created || Created->Num() == 0)
    {
        return FString();
    }

    const TSharedPtr<FJsonObject> Obj = (*Created)[0].IsValid() ? (*Created)[0]->AsObject() : nullptr;
    return Obj.IsValid() && Obj->HasField(TEXT("actor_path")) ? Obj->GetStringField(TEXT("actor_path")) : FString();
}

static bool IsValidationInvalidArgs_Write(const FBridgeResponse& Response)
{
    return BridgeStatusToString(Response.Status) == TEXT("validation_error")
        && HasErrorCode(Response, TEXT("INVALID_ARGS"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_SpawnActor,
    "Project.AgentBridge.L1.Write.SpawnActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_SpawnActor::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetSubsystem_Write(*this);
    if (!Subsystem) return false;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    const FString LevelPath = World ? World->GetPathName() : TEXT("/Game/Maps/TestMap");
    const FString ActorClass = TEXT("/Script/Engine.StaticMeshActor");
    const FString ActorName = TEXT("T1_08_TestCube_01");
    const FBridgeTransform Input = MakeTransform(1000.0f, 2000.0f, 0.0f);

    // 1) 参数校验
    {
        const FBridgeResponse R1 = Subsystem->SpawnActor(TEXT(""), ActorClass, ActorName, Input);
        TestTrue(TEXT("空 LevelPath -> validation_error"), IsValidationInvalidArgs_Write(R1));

        const FBridgeResponse R2 = Subsystem->SpawnActor(LevelPath, TEXT(""), ActorName, Input);
        TestTrue(TEXT("空 ActorClass -> validation_error"), IsValidationInvalidArgs_Write(R2));

        FBridgeTransform ZeroScale = Input;
        ZeroScale.RelativeScale3D = FVector::ZeroVector;
        const FBridgeResponse R3 = Subsystem->SpawnActor(LevelPath, ActorClass, ActorName, ZeroScale);
        TestTrue(TEXT("零缩放 -> validation_error"), IsValidationInvalidArgs_Write(R3));
    }

    // 2) dry_run
    {
        const FString DryRunName = TEXT("T1_08_DryRun_Cube");
        const FBridgeResponse Dry = Subsystem->SpawnActor(LevelPath, ActorClass, DryRunName, Input, true);
        TestEqual(TEXT("dry_run status"), BridgeStatusToString(Dry.Status), TEXT("success"));
        TestFalse(TEXT("dry_run 不应真的生成 Actor"), ContainsActorName(Subsystem, DryRunName));
    }

    // 3) 实际执行
    const FBridgeResponse Resp = Subsystem->SpawnActor(LevelPath, ActorClass, ActorName, Input, false);
    TestEqual(TEXT("spawn status"), BridgeStatusToString(Resp.Status), TEXT("success"));
    TestTrue(TEXT("spawn bTransaction=true"), Resp.bTransaction);
    TestTrue(TEXT("spawn data 存在"), Resp.Data.IsValid());
    if (!Resp.Data.IsValid()) return false;

    const FString SpawnedPath = GetCreatedActorPath(Resp);
    TestFalse(TEXT("created_objects[0].actor_path 非空"), SpawnedPath.IsEmpty());
    TestTrue(TEXT("actual_transform 存在"), Resp.Data->HasField(TEXT("actual_transform")));

    // 4) 写后读回容差
    const TSharedPtr<FJsonObject>* Actual = nullptr;
    if (Resp.Data->TryGetObjectField(TEXT("actual_transform"), Actual) && Actual && (*Actual).IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
        if ((*Actual)->TryGetArrayField(TEXT("location"), Loc) && Loc && Loc->Num() == 3)
        {
            TestNearlyEqual(TEXT("location.X 容差 <= 0.01"), static_cast<float>((*Loc)[0]->AsNumber()), 1000.0f, 0.01f);
            TestNearlyEqual(TEXT("location.Y 容差 <= 0.01"), static_cast<float>((*Loc)[1]->AsNumber()), 2000.0f, 0.01f);
        }
    }

    // 5) dirty_assets
    const TArray<TSharedPtr<FJsonValue>>* Dirty = nullptr;
    TestTrue(TEXT("dirty_assets 非空"), Resp.Data->TryGetArrayField(TEXT("dirty_assets"), Dirty) && Dirty && Dirty->Num() > 0);

    // 6) Undo 后 Actor 消失
    if (GEditor) GEditor->UndoTransaction();
    TestFalse(TEXT("Undo 后 Actor 应消失"), ContainsActorName(Subsystem, ActorName));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_SetActorTransform,
    "Project.AgentBridge.L1.Write.SetActorTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_SetActorTransform::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetSubsystem_Write(*this);
    if (!Subsystem) return false;

    // 1) 参数校验
    {
        const FBridgeResponse Empty = Subsystem->SetActorTransform(TEXT(""), MakeTransform(1, 2, 3));
        TestTrue(TEXT("空 ActorPath -> validation_error"), IsValidationInvalidArgs_Write(Empty));
    }

    // 2) Actor 不存在
    {
        const FBridgeResponse Missing = Subsystem->SetActorTransform(TEXT("/Non/Existent"), MakeTransform(1, 2, 3));
        TestEqual(TEXT("不存在 Actor status=failed"), BridgeStatusToString(Missing.Status), TEXT("failed"));
        TestTrue(TEXT("错误码 ACTOR_NOT_FOUND"), HasErrorCode(Missing, TEXT("ACTOR_NOT_FOUND")));
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    const FString LevelPath = World ? World->GetPathName() : TEXT("/Game/Maps/TestMap");

    // 先生成测试 Actor（SpawnActor 已增加无头安全路径）。
    const FString ActorName = TEXT("T1_09_TransformActor");
    const FBridgeResponse Spawn = Subsystem->SpawnActor(LevelPath, TEXT("/Script/Engine.StaticMeshActor"), ActorName, MakeTransform(100, 100, 0));
    if (!Spawn.IsSuccess())
    {
        AddError(TEXT("SetActorTransform 测试前置 SpawnActor 失败。"));
        return false;
    }

    const FString ActorPath = GetCreatedActorPath(Spawn);
    if (ActorPath.IsEmpty())
    {
        AddError(TEXT("SetActorTransform 测试未能获取 actor_path。"));
        return false;
    }

    // 3) dry_run
    {
        const FBridgeResponse Dry = Subsystem->SetActorTransform(ActorPath, MakeTransform(999, 888, 77), true);
        TestEqual(TEXT("dry_run status"), BridgeStatusToString(Dry.Status), TEXT("success"));
        TestTrue(TEXT("dry_run 返回 old_transform"), Dry.Data.IsValid() && Dry.Data->HasField(TEXT("old_transform")));

        const FBridgeResponse State = Subsystem->GetActorState(ActorPath);
        if (State.IsSuccess() && State.Data.IsValid())
        {
            const TSharedPtr<FJsonObject>* Tr = nullptr;
            if (State.Data->TryGetObjectField(TEXT("transform"), Tr) && Tr && (*Tr).IsValid())
            {
                const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
                if ((*Tr)->TryGetArrayField(TEXT("location"), Loc) && Loc && Loc->Num() == 3)
                {
                    // dry_run 后位置不应被直接改成目标值
                    TestTrue(TEXT("dry_run 后 X 不应等于目标 999"), FMath::Abs(static_cast<float>((*Loc)[0]->AsNumber()) - 999.0f) > 0.01f);
                    TestTrue(TEXT("dry_run 后 Y 不应等于目标 888"), FMath::Abs(static_cast<float>((*Loc)[1]->AsNumber()) - 888.0f) > 0.01f);
                }
            }
        }
    }

    // 4) 实际执行
    const FBridgeTransform NewT = MakeTransform(500, 600, 70, 90.0f, 2.0f);
    const FBridgeResponse SetResp = Subsystem->SetActorTransform(ActorPath, NewT, false);
    TestEqual(TEXT("set status"), BridgeStatusToString(SetResp.Status), TEXT("success"));
    TestTrue(TEXT("set bTransaction=true"), SetResp.bTransaction);
    TestTrue(TEXT("set data 存在"), SetResp.Data.IsValid());

    if (SetResp.Data.IsValid())
    {
        TestTrue(TEXT("含 old_transform"), SetResp.Data->HasField(TEXT("old_transform")));
        TestTrue(TEXT("含 actual_transform"), SetResp.Data->HasField(TEXT("actual_transform")));

        const TSharedPtr<FJsonObject>* OldObj = nullptr;
        const TSharedPtr<FJsonObject>* ActualObj = nullptr;
        if (SetResp.Data->TryGetObjectField(TEXT("old_transform"), OldObj) && SetResp.Data->TryGetObjectField(TEXT("actual_transform"), ActualObj)
            && OldObj && ActualObj && (*OldObj).IsValid() && (*ActualObj).IsValid())
        {
            FString OldJson;
            FString ActualJson;
            {
                TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OldJson);
                FJsonSerializer::Serialize((*OldObj).ToSharedRef(), W);
            }
            {
                TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&ActualJson);
                FJsonSerializer::Serialize((*ActualObj).ToSharedRef(), W);
            }
            TestTrue(TEXT("old_transform != actual_transform"), OldJson != ActualJson);

            const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
            if ((*ActualObj)->TryGetArrayField(TEXT("location"), Loc) && Loc && Loc->Num() == 3)
            {
                TestNearlyEqual(TEXT("actual X 容差 <=0.01"), static_cast<float>((*Loc)[0]->AsNumber()), 500.0f, 0.01f);
                TestNearlyEqual(TEXT("actual Y 容差 <=0.01"), static_cast<float>((*Loc)[1]->AsNumber()), 600.0f, 0.01f);
            }
        }
    }

    // 5) Undo 清理：先撤销 SetActorTransform，再撤销 SpawnActor。
    if (GEditor)
    {
        GEditor->UndoTransaction();
        GEditor->UndoTransaction();
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_ImportAssets,
    "Project.AgentBridge.L1.Write.ImportAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_ImportAssets::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetSubsystem_Write(*this);
    if (!Subsystem) return false;

    // 1) 参数校验
    {
        const FBridgeResponse R1 = Subsystem->ImportAssets(TEXT(""), TEXT("/Game/Imported"));
        TestTrue(TEXT("空 SourceDir -> validation_error"), IsValidationInvalidArgs_Write(R1));

        const FBridgeResponse R2 = Subsystem->ImportAssets(TEXT("D:/NoSource"), TEXT(""));
        TestTrue(TEXT("空 DestPath -> validation_error"), IsValidationInvalidArgs_Write(R2));
    }

    // 2) dry_run（无需真实源文件）
    const FBridgeResponse Dry = Subsystem->ImportAssets(TEXT("D:/NoSource"), TEXT("/Game/Imported"), false, true);
    TestEqual(TEXT("dry_run status"), BridgeStatusToString(Dry.Status), TEXT("success"));
    TestTrue(TEXT("dry_run bTransaction=true"), Dry.bTransaction);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_CreateBlueprintChild,
    "Project.AgentBridge.L1.Write.CreateBlueprintChild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_CreateBlueprintChild::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetSubsystem_Write(*this);
    if (!Subsystem) return false;

    const FString ParentClass = TEXT("/Script/Engine.Actor");
    const FString UniqueSuffix = FString::FromInt(FDateTime::UtcNow().ToUnixTimestamp());
    const FString PackagePath = FString::Printf(TEXT("/Game/Tests/BP_T1_11_TestChild_%s"), *UniqueSuffix);

    // 1) 参数校验
    {
        const FBridgeResponse R1 = Subsystem->CreateBlueprintChild(TEXT(""), PackagePath);
        TestTrue(TEXT("空 ParentClass -> validation_error"), IsValidationInvalidArgs_Write(R1));

        const FBridgeResponse R2 = Subsystem->CreateBlueprintChild(ParentClass, TEXT(""));
        TestTrue(TEXT("空 PackagePath -> validation_error"), IsValidationInvalidArgs_Write(R2));
    }

    // 2) 不存在父类
    {
        const FBridgeResponse Bad = Subsystem->CreateBlueprintChild(TEXT("/Script/NonExistent.Class"), PackagePath);
        TestEqual(TEXT("坏父类 status=failed"), BridgeStatusToString(Bad.Status), TEXT("failed"));
        TestTrue(TEXT("坏父类错误码 CLASS_NOT_FOUND"), HasErrorCode(Bad, TEXT("CLASS_NOT_FOUND")));
    }

    // 3) dry_run
    {
        const FBridgeResponse Dry = Subsystem->CreateBlueprintChild(ParentClass, TEXT("/Game/Tests/BP_T1_11_DryRun"), true);
        TestEqual(TEXT("dry_run status"), BridgeStatusToString(Dry.Status), TEXT("success"));
        TestTrue(TEXT("dry_run bTransaction=true"), Dry.bTransaction);
    }

    // 4) 实际创建 + Undo（无头模式下跳过，避免引擎 BlueprintCompilationManager ensure）
    if (!FApp::IsUnattended())
    {
        const FBridgeResponse Create = Subsystem->CreateBlueprintChild(ParentClass, PackagePath, false);
        TestEqual(TEXT("create status"), BridgeStatusToString(Create.Status), TEXT("success"));
        TestTrue(TEXT("create bTransaction=true"), Create.bTransaction);
        TestTrue(TEXT("created_objects 存在"), Create.Data.IsValid() && Create.Data->HasField(TEXT("created_objects")));

        if (GEditor)
        {
            GEditor->UndoTransaction();
        }
    }
    else
    {
        AddWarning(TEXT("无头模式下跳过 CreateBlueprintChild 的实际创建步骤，仅验证参数/错误路径/dry_run。"));
    }

    return true;
}


