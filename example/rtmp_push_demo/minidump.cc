#include "minidump.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <strsafe.h>

#include <ctime>
#include <vector>
#include <string>

static std::string ExeDir() {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t pos = s.find_last_of("\\/");
    if (pos == std::string::npos) {
        return ".\\";
    }
    return s.substr(0, pos + 1);
}

static std::string NowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        // Ignore debug breakpoint exceptions (0x80000003) - these are typically assert failures
        // that should be handled by the debugger, not reported as crashes
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    
    const std::string dir = ExeDir();
    const std::string stamp = NowStamp();
    const std::string dump_path = dir + "crash_" + stamp + ".dmp";
    const std::string txt_path = dir + "crash_" + stamp + ".txt";

    // Resolve fault module
    HMODULE fault_mod = nullptr;
    void* fault_addr = nullptr;
    if (ep && ep->ExceptionRecord) {
        fault_addr = ep->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)fault_addr, &fault_mod);
    }
    char fault_mod_path[MAX_PATH] = {0};
    if (fault_mod) {
        GetModuleFileNameA(fault_mod, fault_mod_path, MAX_PATH);
    }

    HANDLE hFile = CreateFileA(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;

        const MINIDUMP_TYPE dump_type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dump_type, &mei, nullptr,
                          nullptr);
        CloseHandle(hFile);
    }

    // Symbolize stack. NOTE: CaptureStackBackTrace() only captures the filter's stack,
    // not the faulting thread's stack at the exception point. Use StackWalk64 with ContextRecord.
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);

    // Use WriteFile instead of fprintf to avoid CRT issues in exception handler
    HANDLE hTxtFile = CreateFileA(txt_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTxtFile != INVALID_HANDLE_VALUE) {
        char buf[1024];
        DWORD written = 0;
        
        // Use simple string literals and Windows API instead of CRT to avoid crashes in exception handler
        const char* msg = "Unhandled exception.\r\n";
        WriteFile(hTxtFile, msg, (DWORD)lstrlenA(msg), &written, nullptr);
        
        if (ep && ep->ExceptionRecord) {
            StringCchPrintfA(buf, _countof(buf), "code=0x%08X addr=0x%p\r\n",
                             (unsigned)ep->ExceptionRecord->ExceptionCode,
                             ep->ExceptionRecord->ExceptionAddress);
            WriteFile(hTxtFile, buf, (DWORD)lstrlenA(buf), &written, nullptr);
        }
        
        if (fault_mod_path[0] != '\0') {
            StringCchPrintfA(buf, _countof(buf), "module=%s\r\n", fault_mod_path);
            WriteFile(hTxtFile, buf, (DWORD)lstrlenA(buf), &written, nullptr);
            if (fault_mod && fault_addr) {
                StringCchPrintfA(buf, _countof(buf), "module_offset=0x%p\r\n",
                                 (void*)((uintptr_t)fault_addr - (uintptr_t)fault_mod));
                WriteFile(hTxtFile, buf, (DWORD)lstrlenA(buf), &written, nullptr);
            }
        }
        
        StringCchPrintfA(buf, _countof(buf), "dump=%s\r\n", dump_path.c_str());
        WriteFile(hTxtFile, buf, (DWORD)lstrlenA(buf), &written, nullptr);
        
        // Stack trace (from exception context)
        if (ep && ep->ContextRecord) {
            CONTEXT ctx = *ep->ContextRecord;
            STACKFRAME64 frame{};
            DWORD machine = 0;
#if defined(_M_IX86)
            machine = IMAGE_FILE_MACHINE_I386;
            frame.AddrPC.Offset = ctx.Eip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = ctx.Ebp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx.Esp;
            frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_X64)
            machine = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset = ctx.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = ctx.Rsp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
#else
            machine = 0;
#endif

            int frame_count = 0;
            while (machine != 0 &&
                   StackWalk64(machine, proc, GetCurrentThread(), &frame, &ctx, nullptr,
                              SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
                   frame.AddrPC.Offset != 0 &&
                   frame_count < 64) {
                DWORD64 addr = frame.AddrPC.Offset;

                // module + offset
                HMODULE m = nullptr;
                char mod_path[MAX_PATH] = {0};
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)addr, &m);
                if (m) {
                    GetModuleFileNameA(m, mod_path, MAX_PATH);
                }
                void* mod_off = m ? (void*)((uintptr_t)addr - (uintptr_t)m) : nullptr;

                // symbol
                char sym_buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
                SYMBOL_INFO* sym = (SYMBOL_INFO*)sym_buf;
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = MAX_SYM_NAME;
                DWORD64 disp = 0;
                bool has_sym = SymFromAddr(proc, addr, &disp, sym) != FALSE;

                // line
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD line_disp = 0;
                bool has_line = SymGetLineFromAddr64(proc, addr, &line_disp, &line) != FALSE;

                if (mod_path[0] != '\0') {
                    if (has_sym && has_line) {
                        StringCchPrintfA(buf, _countof(buf),
                                         "#%02d %s+0x%p %s+0x%p (%s:%lu)\r\n",
                                         frame_count, mod_path, mod_off, sym->Name,
                                         (void*)(uintptr_t)disp, line.FileName, (unsigned long)line.LineNumber);
                    } else if (has_sym) {
                        StringCchPrintfA(buf, _countof(buf),
                                         "#%02d %s+0x%p %s+0x%p (0x%p)\r\n",
                                         frame_count, mod_path, mod_off, sym->Name,
                                         (void*)(uintptr_t)disp, (void*)(uintptr_t)addr);
                    } else {
                        StringCchPrintfA(buf, _countof(buf),
                                         "#%02d %s+0x%p (0x%p)\r\n",
                                         frame_count, mod_path, mod_off, (void*)(uintptr_t)addr);
                    }
                } else {
                    if (has_sym) {
                        StringCchPrintfA(buf, _countof(buf),
                                         "#%02d %s+0x%p (0x%p)\r\n",
                                         frame_count, sym->Name, (void*)(uintptr_t)disp, (void*)(uintptr_t)addr);
                    } else {
                        StringCchPrintfA(buf, _countof(buf),
                                         "#%02d (0x%p)\r\n", frame_count, (void*)(uintptr_t)addr);
                    }
                }
                WriteFile(hTxtFile, buf, (DWORD)lstrlenA(buf), &written, nullptr);
                frame_count++;
            }

            StringCchPrintfA(buf, _countof(buf), "stack_frames=%d\r\n", frame_count);
            WriteFile(hTxtFile, buf, (DWORD)lstrlenA(buf), &written, nullptr);
        }
        
        CloseHandle(hTxtFile);
    }

    SymCleanup(proc);
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallMiniDumpHandler() {
    // Ensure dbghelp can find symbols if present; still writes dump without.
    SetUnhandledExceptionFilter(UnhandledFilter);
}


