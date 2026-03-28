"""
uat_runner.py
=============
AGENT + UE5 可操作層 — UAT (Unreal Automation Tool) Python 封装。

UE5 官方模块：UAT

本模块提供从 Python 端调用 UAT 的能力，供 Orchestrator 和 CLI 使用。
与 C++ 端的 FUATRunner 功能对等，但运行在 Editor 进程外部。

使用场景：
  - Python CLI（agent-ue5 build / agent-ue5 test）
  - Python Orchestrator 中触发构建
  - CI/CD 脚本

注意：UAT 运行在引擎进程外部（C# 程序），
本模块通过 subprocess 启动 UAT 进程。
"""

from __future__ import annotations

import os
import platform
import subprocess
from dataclasses import dataclass

try:
    from project_config import get_saved_dir, get_uproject_path
except ImportError:  # pragma: no cover - 兼容包导入
    from .project_config import get_saved_dir, get_uproject_path


# ============================================================
# 运行结果
# ============================================================

@dataclass
class UATRunResult:
    """UAT 子进程运行结果。对应 C++ 端的 FUATRunResult。"""
    launched: bool = False
    completed: bool = False
    exit_code: int = -1
    command_line: str = ""
    stdout: str = ""
    stderr: str = ""
    error_message: str = ""

    @property
    def is_success(self) -> bool:
        return self.launched and self.completed and self.exit_code == 0


# ============================================================
# UAT Runner
# ============================================================

class UATRunner:
    """
    UAT 子进程封装（Python 版）。
    对应 C++ 端的 FUATRunner。
    """

    def __init__(self, project_path: str = "", engine_dir: str = ""):
        """
        初始化 UAT Runner。

        参数：
            project_path: .uproject 文件路径（空=自动检测）
            engine_dir: 引擎目录（可传 UE 安装根目录，或直接传 .../Engine 目录）
        """
        self.project_path = project_path or str(get_uproject_path())
        self.engine_dir = engine_dir
        self._run_uat_path = self._detect_run_uat_path()

    # ============================================================
    # 核心操作
    # ============================================================

    def build_cook_run(
        self,
        platform_name: str = "Win64",
        configuration: str = "Development",
        sync: bool = True,
    ) -> UATRunResult:
        """
        执行 BuildCookRun。
        对应 UAT 命令：RunUAT BuildCookRun -project=... -build -cook -stage -pak
        """
        args = (
            f"BuildCookRun"
            f' -project="{self.project_path}"'
            f" -platform={platform_name}"
            f" -clientconfig={configuration}"
            f" -build -cook -stage -pak"
            f" -unattended -utf8output"
        )
        return self._execute_uat(args, sync=sync)

    def run_automation_tests(
        self,
        test_filter: str = "Project.AgentBridge",
        report_path: str = "",
        sync: bool = True,
    ) -> UATRunResult:
        """
        通过 UAT 运行自动化测试。
        UE5.5 中不再依赖独立的 RunAutomationTests 子命令，
        统一走 BuildCookRun 的 editortest 路径：
        RunUAT BuildCookRun -project=... -run -editortest -RunAutomationTest=...
        """
        args = (
            f"BuildCookRun"
            f' -project="{self.project_path}"'
            f" -run -editortest -unattended -utf8output -nullrhi -NoP4"
        )
        if test_filter:
            args += f" -RunAutomationTest={test_filter}"
        else:
            args += " -RunAutomationTests"

        if report_path:
            resolved_report_path = self._resolve_report_path(report_path)
            # BuildCookRun 没有稳定的直接报告参数，这里通过附加命令行透传给 Automation。
            args += f' -addcmdline="-ReportExportPath=\\"{resolved_report_path}\\""'
        return self._execute_uat(args, sync=sync)

    def run_gauntlet(
        self,
        test_config_name: str = "AgentBridge.AllTests",
        sync: bool = True,
    ) -> UATRunResult:
        """
        启动 Gauntlet 测试会话。
        对应 UAT 命令：RunUAT RunGauntlet -Test=... -project=...
        """
        args = (
            f"RunGauntlet"
            f' -project="{self.project_path}"'
            f" -Test={test_config_name}"
            f" -unattended -utf8output"
        )
        return self._execute_uat(args, sync=sync)

    def run_custom(self, uat_command: str, sync: bool = True) -> UATRunResult:
        """执行任意 UAT 命令。"""
        args = f'{uat_command} -project="{self.project_path}" -unattended -utf8output'
        return self._execute_uat(args, sync=sync)

    # ============================================================
    # 状态检查
    # ============================================================

    @property
    def is_available(self) -> bool:
        """检查 RunUAT 是否存在。"""
        return self._run_uat_path != "" and os.path.isfile(self._run_uat_path)

    @property
    def run_uat_path(self) -> str:
        """获取 RunUAT 路径。"""
        return self._run_uat_path

    # ============================================================
    # 内部方法
    # ============================================================

    def _execute_uat(self, args: str, sync: bool = True) -> UATRunResult:
        """执行 UAT 子进程。"""
        result = UATRunResult()

        if not self.is_available:
            result.error_message = f"RunUAT not found at: {self._run_uat_path}"
            return result

        full_command = f'"{self._run_uat_path}" {args}'
        result.command_line = full_command

        try:
            if sync:
                proc = subprocess.run(
                    full_command,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=3600,  # 1 小时超时
                )
                result.launched = True
                result.completed = True
                result.exit_code = proc.returncode
                result.stdout = proc.stdout
                result.stderr = proc.stderr
            else:
                proc = subprocess.Popen(
                    full_command,
                    shell=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                result.launched = True
                result.completed = False
                result.exit_code = -1

        except subprocess.TimeoutExpired:
            result.launched = True
            result.completed = False
            result.error_message = "UAT process timed out after 3600 seconds"
        except Exception as e:
            result.error_message = f"Failed to launch UAT: {e}"

        return result

    def _detect_run_uat_path(self) -> str:
        """自动检测 RunUAT 路径。"""
        if self.engine_dir:
            base = self.engine_dir
        else:
            # 尝试从环境变量获取
            base = os.environ.get("UE_ENGINE_DIR", "")

        if not base:
            return ""

        if platform.system() == "Windows":
            candidate_paths = [
                os.path.join(base, "Build", "BatchFiles", "RunUAT.bat"),
                os.path.join(base, "Engine", "Build", "BatchFiles", "RunUAT.bat"),
            ]
        else:
            candidate_paths = [
                os.path.join(base, "Build", "BatchFiles", "RunUAT.sh"),
                os.path.join(base, "Engine", "Build", "BatchFiles", "RunUAT.sh"),
            ]

        for path in candidate_paths:
            if os.path.isfile(path):
                return os.path.abspath(path)

        return ""

    def _resolve_report_path(self, report_path: str) -> str:
        """将相对测试报告路径解析到项目 Saved/ 下。"""
        if os.path.isabs(report_path):
            return report_path
        return str(get_saved_dir() / report_path)

