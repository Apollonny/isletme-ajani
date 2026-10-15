#include "ajan.hpp"
#include <iostream>
static void schema_check(J value, J schema) {
    if (!value.is_object())
        throw std::runtime_error("Plugin arguments must be an object");
    for (auto &r : schema.value("required", J::array()))
        if (!value.contains(r.get<std::string>()))
            throw std::runtime_error("Required plugin argument missing");
    auto properties = schema.value("properties", J::object());
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!properties.contains(it.key()))
            throw std::runtime_error("Unexpected plugin argument");
        auto type = properties[it.key()].value("type", "");
        if ((type == "string" && !it->is_string()) ||
            (type == "integer" && !it->is_number_integer()) ||
            (type == "boolean" && !it->is_boolean()))
            throw std::runtime_error("Plugin argument type mismatch");
        if (it->is_string() && it->get<std::string>().size() > 1024)
            throw std::runtime_error("Plugin argument size limit");
    }
}
J plugin_run(J spec, J args) {
    schema_check(args, spec.at("input_schema"));
    auto base = fs::weakly_canonical("plugins");
    auto executable = fs::weakly_canonical(spec.at("executable").get<std::string>());
    auto relative = fs::relative(executable, base);
    if (relative.empty() || relative.native().find(L"..") != std::wstring::npos ||
        executable.extension() != L".exe")
        throw std::runtime_error("Plugin executable must be inside plugins directory");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE childIn, writeIn, readOut, childOut;
    CreatePipe(&childIn, &writeIn, &sa, 0);
    CreatePipe(&readOut, &childOut, &sa, 0);
    SetHandleInformation(writeIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(readOut, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = childIn;
    si.hStdOutput = childOut;
    si.hStdError = childOut;
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + executable.native() + L"\"";
    HANDLE job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limit.ProcessMemoryLimit = 128 * 1024 * 1024;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
    bool created = CreateProcessW(executable.c_str(), command.data(), 0, 0, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED, 0, base.c_str(), &si, &pi);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (!created) {
        CloseHandle(writeIn);
        CloseHandle(readOut);
        CloseHandle(job);
        throw std::runtime_error("Plugin launch failed");
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    std::string input = J({{"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", spec.value("method", "execute")},
                           {"params", args}})
                            .dump() +
                        "\n";
    DWORD n;
    WriteFile(writeIn, input.data(), (DWORD)input.size(), &n, 0);
    CloseHandle(writeIn);
    std::string output;
    double start = clock_ms();
    while (clock_ms() - start < spec.value("timeout_ms", 3000)) {
        DWORD available = 0;
        if (PeekNamedPipe(readOut, 0, 0, 0, &available, 0) && available) {
            char b[4096];
            ReadFile(readOut, b, std::min<DWORD>(available, sizeof b), &n, 0);
            output.append(b, n);
            if (output.size() > 65536)
                break;
            if (output.find('\n') != std::string::npos)
                break;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0 && !available)
            break;
        Sleep(5);
    }
    CloseHandle(readOut);
    CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (output.empty() || output.size() > 65536)
        throw std::runtime_error("Plugin timeout/output limit");
    auto response = J::parse(output);
    if (response.value("jsonrpc", "") != "2.0" || response.value("id", 0) != 1 ||
        !response.contains("result"))
        throw std::runtime_error("Invalid JSON-RPC plugin response");
    auto result = response["result"];
    if (!result.is_object() || !result.contains("message"))
        throw std::runtime_error("Plugin must provide a customer-visible evidence message");
    return {{"ok", true},
            {"message", result["message"]},
            {"data", result},
            {"source", "plugin:" + spec.at("name").get<std::string>()}};
}
