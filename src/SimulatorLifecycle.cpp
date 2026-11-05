#include "SimulatorLifecycle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>
#else
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
std::wstring wideName(const char *name) {
  return std::wstring(name, name + std::strlen(name));
}

// Both tables must change: the CRT's (what getenv reads) and the process block
// (what the next process gets).
void unsetenv(const char *name) {
  const std::wstring wide = wideName(name);
  _wputenv_s(wide.c_str(), L"");
  SetEnvironmentVariableW(wide.c_str(), nullptr);
}

void setenvWide(const char *name, const std::wstring &value) {
  const std::wstring wide = wideName(name);
  _wputenv_s(wide.c_str(), value.c_str());
  SetEnvironmentVariableW(wide.c_str(), value.c_str());
}

// Only for the decimal reboot tokens, so plain ASCII.
void setenv(const char *name, const char *value, int /*overwrite*/) {
  setenvWide(name, wideName(value));
}
#endif

constexpr const char *kWakeReasonEnv = "CROSSPOINT_SIM_WAKE_REASON";
constexpr const char *kInputScriptEnv = "CROSSPOINT_SIM_INPUT_SCRIPT";
constexpr const char *kInputScriptAfterWakeEnv =
    "CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE";
constexpr const char *kScreenshotsEnv = "CROSSPOINT_SIM_SCREENSHOTS";
constexpr const char *kScreenshotsAfterWakeEnv =
    "CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE";
constexpr const char *kInputScriptAfterRebootEnv =
    "CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_REBOOT";
constexpr const char *kScreenshotsAfterRebootEnv =
    "CROSSPOINT_SIM_SCREENSHOTS_AFTER_REBOOT";
constexpr const char *kSilentRebootMagicEnv =
    "CROSSPOINT_SIM_SILENT_REBOOT_MAGIC";
constexpr const char *kSilentRebootTargetEnv =
    "CROSSPOINT_SIM_SILENT_REBOOT_TARGET";
constexpr const char *kSilentRebootPayloadEnv =
    "CROSSPOINT_SIM_SILENT_REBOOT_PAYLOAD";
char **gArgv = nullptr;

void promoteAfterWakeValue(const char *target, const char *afterWake) {
#ifdef _WIN32
  // Wide, because a schedule can hold a non-ASCII screenshot path.
  const wchar_t *value = _wgetenv(wideName(afterWake).c_str());
  if (value) {
    setenvWide(target, std::wstring(value));
  } else {
    unsetenv(target);
  }
#else
  const char *value = std::getenv(afterWake);
  if (value) {
    setenv(target, value, 1);
  } else {
    unsetenv(target);
  }
#endif
  unsetenv(afterWake);
}

uint32_t readToken(const char *name) {
  const char* value = std::getenv(name);
  return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0;
}

void clearSilentRebootToken() {
  unsetenv(kSilentRebootMagicEnv);
  unsetenv(kSilentRebootTargetEnv);
  unsetenv(kSilentRebootPayloadEnv);
}

#ifdef _WIN32

// exec() replaces a process atomically. Windows has nothing like it: its _execvp
// starts a new process and ends the old one, so whoever started the simulator
// sees a success exit at the first reboot, and starting the new image while the
// old one lives would leave its threads running and its SD files open.
//
// So the process the user starts only supervises. It runs the simulator as a
// child; a reboot sends the child's environment back over a pipe and terminates
// the child, and the supervisor starts a fresh one with that environment. It
// stays alive through reboots and exits with the final child's exit code. A job
// object takes the child down with the supervisor if the supervisor is killed.
constexpr const wchar_t *kSupervisorPipeEnv = L"CROSSPOINT_SIM_SUPERVISOR_PIPE";
constexpr UINT kRelaunchExitCode = 0x5EB0071;
HANDLE gSupervisorPipe = nullptr;

// Sends the environment block as is. It is double-NUL terminated.
[[noreturn]] void reexec() {
  wchar_t *block = GetEnvironmentStringsW();
  size_t length = 0;
  while (block[length] != L'\0' || block[length + 1] != L'\0')
    ++length;
  length += 2;

  std::fflush(stdout);
  std::fflush(stderr);
  DWORD written = 0;
  const DWORD bytes = static_cast<DWORD>(length * sizeof(wchar_t));
  if (!gSupervisorPipe ||
      !WriteFile(gSupervisorPipe, block, bytes, &written, nullptr) ||
      written != bytes) {
    std::fputs("SimulatorLifecycle: cannot reach the supervisor\n", stderr);
    TerminateProcess(GetCurrentProcess(), 1);
  }
  // Stops every thread at once and releases all files, as exec does.
  TerminateProcess(GetCurrentProcess(), kRelaunchExitCode);
  std::abort(); // Unreachable: TerminateProcess on self does not return.
}

// Adds the pipe variable, which the child removed, keeping the block sorted as
// CreateProcess documents it must be.
std::vector<wchar_t> childEnvironment(const std::vector<wchar_t> &base,
                                      HANDLE pipe) {
  const std::wstring entry =
      std::wstring(kSupervisorPipeEnv) + L"=" +
      std::to_wstring(reinterpret_cast<uintptr_t>(pipe));
  size_t position = 0;
  while (position < base.size() && base[position] != L'\0' &&
         _wcsicmp(base.data() + position, entry.c_str()) < 0) {
    position += wcslen(base.data() + position) + 1;
  }
  std::vector<wchar_t> out(base.begin(), base.begin() + position);
  out.insert(out.end(), entry.begin(), entry.end());
  out.push_back(L'\0');
  out.insert(out.end(), base.begin() + position, base.end());
  return out;
}

#else

[[noreturn]] void reexec() {
  if (!gArgv || !gArgv[0]) {
    std::fputs("SimulatorLifecycle: missing argv for reboot\n", stderr);
    _exit(1);
  }

  execvp(gArgv[0], gArgv);

  std::perror("execvp");
  _exit(1);
}

#endif

} // namespace

namespace SimulatorLifecycle {

void initProcessArgs(char **argv) { gArgv = argv; }

#ifdef _WIN32
void superviseIfNeeded() {
  // No crash or "drive not ready" dialog may ever block an unattended run. The
  // child inherits this mode.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);

  wchar_t pipeValue[32];
  const DWORD pipeLength = GetEnvironmentVariableW(kSupervisorPipeEnv, pipeValue,
                                                   sizeof(pipeValue) /
                                                       sizeof(pipeValue[0]));
  if (pipeLength > 0 && pipeLength < sizeof(pipeValue) / sizeof(pipeValue[0])) {
    // Child role. Neither curl nor the firmware should see any of this.
    gSupervisorPipe =
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wtoi64(pipeValue)));
    SetHandleInformation(gSupervisorPipe, HANDLE_FLAG_INHERIT, 0);
    _wputenv_s(kSupervisorPipeEnv, L"");
    SetEnvironmentVariableW(kSupervisorPipeEnv, nullptr);
    return;
  }

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                            sizeof(limits));
  }

  wchar_t exePath[32768];
  GetModuleFileNameW(nullptr, exePath, 32768);

  wchar_t *inherited = GetEnvironmentStringsW();
  size_t inheritedLength = 0;
  while (inherited[inheritedLength] != L'\0' ||
         inherited[inheritedLength + 1] != L'\0')
    ++inheritedLength;
  std::vector<wchar_t> environment(inherited, inherited + inheritedLength + 2);
  FreeEnvironmentStringsW(inherited);

  while (true) {
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
      std::fputs("SimulatorLifecycle: cannot create supervisor pipe\n", stderr);
      ExitProcess(1);
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    std::vector<wchar_t> childEnv = childEnvironment(environment, writeEnd);
    std::wstring commandLine = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exePath, commandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                        childEnv.data(), nullptr, &startup, &info)) {
      std::fprintf(stderr, "SimulatorLifecycle: cannot start child (%lu)\n",
                   GetLastError());
      ExitProcess(1);
    }
    if (job)
      AssignProcessToJobObject(job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    CloseHandle(writeEnd);

    // Ends when the child dies, whatever the reason: it owns the only write end.
    std::vector<char> received;
    char buffer[16384];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0)
      received.insert(received.end(), buffer, buffer + got);
    CloseHandle(readEnd);

    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);

    if (exitCode != kRelaunchExitCode ||
        received.size() < 2 * sizeof(wchar_t)) {
      ExitProcess(exitCode);
    }
    environment.assign(
        reinterpret_cast<const wchar_t *>(received.data()),
        reinterpret_cast<const wchar_t *>(received.data() + received.size()));
  }
}
#endif

WakeReason consumeWakeReason() {
  const char *value = std::getenv(kWakeReasonEnv);
  if (!value) {
    return WakeReason::None;
  }

  unsetenv(kWakeReasonEnv);
  if (std::strcmp(value, "power") == 0) {
    return WakeReason::PowerButton;
  }
  return WakeReason::None;
}

void setSilentRebootToken(const uint32_t magic, const uint32_t target, const uint32_t payload) {
  char value[11];
  std::snprintf(value, sizeof(value), "%u", magic);
  setenv(kSilentRebootMagicEnv, value, 1);
  std::snprintf(value, sizeof(value), "%u", target);
  setenv(kSilentRebootTargetEnv, value, 1);
  std::snprintf(value, sizeof(value), "%u", payload);
  setenv(kSilentRebootPayloadEnv, value, 1);
}

void restoreSilentRebootToken(uint32_t& magic, uint32_t& target, uint32_t& payload) {
  magic = readToken(kSilentRebootMagicEnv);
  target = readToken(kSilentRebootTargetEnv);
  payload = readToken(kSilentRebootPayloadEnv);
  clearSilentRebootToken();
}

[[noreturn]] void reboot() {
  // ESP.restart() relaunches the process with the same environment. Without
  // this, a scripted flow that triggers a firmware reboot (for example a
  // network-boot handoff) would replay its script from the start forever.
  // Tests that need to keep driving the relaunched instance provide the
  // optional *_AFTER_REBOOT schedules.
  promoteAfterWakeValue(kInputScriptEnv, kInputScriptAfterRebootEnv);
  promoteAfterWakeValue(kScreenshotsEnv, kScreenshotsAfterRebootEnv);
  reexec();
}

[[noreturn]] void rebootAsPowerWake() {
  clearSilentRebootToken();
  setenv(kWakeReasonEnv, "power", 1);
  // A deep-sleep wake is a fresh process. Do not replay the pre-sleep script,
  // which would otherwise put every relaunched process back to sleep forever.
  // Tests can provide an explicit post-wake schedule when they need to capture
  // or terminate the relaunched instance.
  promoteAfterWakeValue(kInputScriptEnv, kInputScriptAfterWakeEnv);
  promoteAfterWakeValue(kScreenshotsEnv, kScreenshotsAfterWakeEnv);
  reexec();
}

} // namespace SimulatorLifecycle
