// L1_QueryTests.cpp
// L1 查询接口自动化测试（7 个）。

#include "Misc/AutomationTest.h"
#include "AgentBridgeSubsystem.h"
#include "BridgeTypes.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
// 获取 Query 测试专用 Subsystem，避免 Unity Build 下与其他测试文件的同名辅助函数冲突。
static UAgentBridgeSubsystem* GetQueryTestSubsystem(FAutomationTestBase& Test)
{
    UAgentBridgeSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAgentBridgeSubsystem>() : nullptr;
    if (!Subsystem)
    {
        Test.AddError(TEXT("AgentBridgeSubsystem 不可用，请确认 AgentBridge 插件已启用。"));
    }
    return Subsystem;
}

static bool IsValidationInvalidArgs(const FBridgeResponse& Response)
{
    return BridgeStatusToString(Response.Status) == TEXT("validation_error")
        && Response.Errors.Num() > 0
        && Response.Errors[0].Code == TEXT("INVALID_ARGS");
}

static FString GetAnyActorPath(UAgentBridgeSubsystem* Subsystem)
{
    const FBridgeResponse ListResp = Subsystem->ListLevelActors();
    if (!ListResp.IsSuccess() || !ListResp.Data.IsValid())
    {
        return FString();
    }

    const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
    if (!ListResp.Data->TryGetArrayField(TEXT("actors"), Actors) || !Actors || Actors->Num() == 0)
    {
        return FString();
    }

    const TSharedPtr<FJsonObject> First = (*Actors)[0].IsValid() ? (*Actors)[0]->AsObject() : nullptr;
    return First.IsValid() ? First->GetStringField(TEXT("actor_path")) : FString();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_GetCurrentProjectState,
    "Project.AgentBridge.L1.Query.GetCurrentProjectState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_GetCurrentProjectState::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetQueryTestSubsystem(*this);
    if (!Subsystem) return false;

    const FBridgeResponse Resp = Subsystem->GetCurrentProjectState();

    TestEqual(TEXT("status"), BridgeStatusToString(Resp.Status), TEXT("success"));
    TestTrue(TEXT("data 存在"), Resp.Data.IsValid());
    if (!Resp.Data.IsValid()) return false;

    TestTrue(TEXT("project_name 非空"), Resp.Data->HasField(TEXT("project_name")) && !Resp.Data->GetStringField(TEXT("project_name")).IsEmpty());
    TestTrue(TEXT("engine_version 含 5.5"), Resp.Data->HasField(TEXT("engine_version")) && Resp.Data->GetStringField(TEXT("engine_version")).Contains(TEXT("5.5")));
    TestTrue(TEXT("current_level 存在"), Resp.Data->HasField(TEXT("current_level")));
    TestTrue(TEXT("editor_mode 合法"),
        Resp.Data->HasField(TEXT("editor_mode"))
        && (Resp.Data->GetStringField(TEXT("editor_mode")) == TEXT("editing") || Resp.Data->GetStringField(TEXT("editor_mode")) == TEXT("pie")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_ListLevelActors,
    "Project.AgentBridge.L1.Query.ListLevelActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_ListLevelActors::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetQueryTestSubsystem(*this);
    if (!Subsystem) return false;

    const FBridgeResponse Resp = Subsystem->ListLevelActors();

    TestEqual(TEXT("status"), BridgeStatusToString(Resp.Status), TEXT("success"));
    TestTrue(TEXT("data 存在"), Resp.Data.IsValid());
    if (!Resp.Data.IsValid()) return false;

    const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
    TestTrue(TEXT("actors 是数组"), Resp.Data->TryGetArrayField(TEXT("actors"), Actors));
    if (!Actors) return false;

    // 空关卡允许空数组，非空时检查字段结构。
    for (const TSharedPtr<FJsonValue>& Item : *Actors)
    {
        const TSharedPtr<FJsonObject> Obj = Item.IsValid() ? Item->AsObject() : nullptr;
        TestTrue(TEXT("actor_name 存在"), Obj.IsValid() && Obj->HasField(TEXT("actor_name")));
        TestTrue(TEXT("actor_path 存在"), Obj.IsValid() && Obj->HasField(TEXT("actor_path")));
        TestTrue(TEXT("class 存在"), Obj.IsValid() && Obj->HasField(TEXT("class")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_GetActorState,
    "Project.AgentBridge.L1.Query.GetActorState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_GetActorState::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetQueryTestSubsystem(*this);
    if (!Subsystem) return false;

    const FString ActorPath = GetAnyActorPath(Subsystem);
    if (ActorPath.IsEmpty())
    {
        AddWarning(TEXT("当前关卡无 Actor，跳过 GetActorState 正常路径断言。"));
    }
    else
    {
        const FBridgeResponse OkResp = Subsystem->GetActorState(ActorPath);
        TestEqual(TEXT("正常路径 status"), BridgeStatusToString(OkResp.Status), TEXT("success"));
        TestTrue(TEXT("data 存在"), OkResp.Data.IsValid());
        if (OkResp.Data.IsValid())
        {
            TestTrue(TEXT("actor_name"), OkResp.Data->HasField(TEXT("actor_name")));
            TestTrue(TEXT("actor_path"), OkResp.Data->HasField(TEXT("actor_path")));
            TestTrue(TEXT("class"), OkResp.Data->HasField(TEXT("class")));
            TestTrue(TEXT("transform"), OkResp.Data->HasField(TEXT("transform")));
            TestTrue(TEXT("collision"), OkResp.Data->HasField(TEXT("collision")));
            TestTrue(TEXT("tags"), OkResp.Data->HasField(TEXT("tags")));
        }
    }

    const FBridgeResponse EmptyResp = Subsystem->GetActorState(TEXT(""));
    TestTrue(TEXT("空参数 -> validation_error + INVALID_ARGS"), IsValidationInvalidArgs(EmptyResp));

    const FBridgeResponse NotFoundResp = Subsystem->GetActorState(TEXT("/Game/Maps/TestMap.NonExistent"));
    TestEqual(TEXT("不存在路径 status"), BridgeStatusToString(NotFoundResp.Status), TEXT("failed"));
    TestTrue(TEXT("不存在路径错误码 ACTOR_NOT_FOUND"), NotFoundResp.Errors.Num() > 0 && NotFoundResp.Errors[0].Code == TEXT("ACTOR_NOT_FOUND"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_GetActorBounds,
    "Project.AgentBridge.L1.Query.GetActorBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_GetActorBounds::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetQueryTestSubsystem(*this);
    if (!Subsystem) return false;

    const FString ActorPath = GetAnyActorPath(Subsystem);
    if (ActorPath.IsEmpty())
    {
        AddWarning(TEXT("当前关卡无 Actor，跳过 GetActorBounds 正常路径断言。"));
    }
    else
    {
        const FBridgeResponse OkResp = Subsystem->GetActorBounds(ActorPath);
        TestEqual(TEXT("正常路径 status"), BridgeStatusToString(OkResp.Status), TEXT("success"));
        TestTrue(TEXT("data 存在"), OkResp.Data.IsValid());
        if (OkResp.Data.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Origin = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Extent = nullptr;
            TestTrue(TEXT("world_bounds_origin[3]"), OkResp.Data->TryGetArrayField(TEXT("world_bounds_origin"), Origin) && Origin && Origin->Num() == 3);
            TestTrue(TEXT("world_bounds_extent[3]"), OkResp.Data->TryGetArrayField(TEXT("world_bounds_extent"), Extent) && Extent && Extent->Num() == 3);
        }
    }

    const FBridgeResponse EmptyResp = Subsystem->GetActorBounds(TEXT(""));
    TestTrue(TEXT("空参数 -> validation_error + INVALID_ARGS"), IsValidationInvalidArgs(EmptyResp));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_GetAssetMetadata,
    "Project.AgentBridge.L1.Query.GetAssetMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_GetAssetMetadata::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetQueryTestSubsystem(*this);
    if (!Subsystem) return false;

    // 正常路径：引擎内置资源。
    const FBridgeResponse ExistsResp = Subsystem->GetAssetMetadata(TEXT("/Engine/BasicShapes/Cube"));
    TestEqual(TEXT("存在资源 status"), BridgeStatusToString(ExistsResp.Status), TEXT("success"));
    TestTrue(TEXT("data 存在"), ExistsResp.Data.IsValid());
    if (ExistsResp.Data.IsValid())
    {
        TestTrue(TEXT("exists=true"), ExistsResp.Data->HasField(TEXT("exists")) && ExistsResp.Data->GetBoolField(TEXT("exists")));
        TestTrue(TEXT("class 非空"), ExistsResp.Data->HasField(TEXT("class")) && !ExistsResp.Data->GetStringField(TEXT("class")).IsEmpty());
    }

    // 不存在资源：应 success + exists=false。
    const FBridgeResponse NotExistsResp = Subsystem->GetAssetMetadata(TEXT("/Game/NonExistent/Asset"));
    TestEqual(TEXT("不存在资源 status 仍为 success"), BridgeStatusToString(NotExistsResp.Status), TEXT("success"));
    TestTrue(TEXT("exists=false"), NotExistsResp.Data.IsValid() && NotExistsResp.Data->HasField(TEXT("exists")) && !NotExistsResp.Data->GetBoolField(TEXT("exists")));

    const FBridgeResponse EmptyResp = Subsystem->GetAssetMetadata(TEXT(""));
    TestTrue(TEXT("空参数 -> validation_error + INVALID_ARGS"), IsValidationInvalidArgs(EmptyResp));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_GetDirtyAssets,
    "Project.AgentBridge.L1.Query.GetDirtyAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_GetDirtyAssets::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetQueryTestSubsystem(*this);
    if (!Subsystem) return false;

    const FBridgeResponse Resp = Subsystem->GetDirtyAssets();
    TestEqual(TEXT("status"), BridgeStatusToString(Resp.Status), TEXT("success"));
    TestTrue(TEXT("data 存在"), Resp.Data.IsValid());

    const TArray<TSharedPtr<FJsonValue>>* Dirty = nullptr;
    TestTrue(TEXT("dirty_assets 是数组"), Resp.Data.IsValid() && Resp.Data->TryGetArrayField(TEXT("dirty_assets"), Dirty));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBridgeL1_RunMapCheck,
    "Project.AgentBridge.L1.Query.RunMapCheck",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBridgeL1_RunMapCheck::RunTest(const FString& Parameters)
{
    UAgentBridgeSubsystem* Subsystem = GetQueryTestSubsystem(*this);
    if (!Subsystem) return false;

    const FBridgeResponse Resp = Subsystem->RunMapCheck();

    TestEqual(TEXT("status"), BridgeStatusToString(Resp.Status), TEXT("success"));
    TestTrue(TEXT("data 存在"), Resp.Data.IsValid());
    if (!Resp.Data.IsValid()) return false;

    TestTrue(TEXT("map_errors 存在"), Resp.Data->HasField(TEXT("map_errors")));
    TestTrue(TEXT("map_warnings 存在"), Resp.Data->HasField(TEXT("map_warnings")));

    return true;
}
