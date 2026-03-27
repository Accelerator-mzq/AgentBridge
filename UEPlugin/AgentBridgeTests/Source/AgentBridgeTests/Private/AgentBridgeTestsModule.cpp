// AgentBridgeTestsModule.cpp
// 仅注册测试模块。

#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"

class FAgentBridgeTestsModule : public IModuleInterface
{
};

IMPLEMENT_MODULE(FAgentBridgeTestsModule, AgentBridgeTests)
