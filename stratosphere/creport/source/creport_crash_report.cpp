#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <switch.h>

#include "creport_crash_report.hpp"
#include "creport_debug_types.hpp"

void CrashReport::EnsureReportDirectories() {
    char path[FS_MAX_PATH];  
    strcpy(path, "sdmc:/atmosphere");
    mkdir(path, S_IRWXU);
    strcat(path, "/crash reports");
    mkdir(path, S_IRWXU);
    strcat(path, "/dumps");
    mkdir(path, S_IRWXU);
}

void CrashReport::SaveReport() {
    /* TODO: Save the report to the SD card. */
    char report_path[FS_MAX_PATH];
    
    /* Ensure path exists. */
    EnsureReportDirectories();
    
    /* Get a timestamp. */
    u64 timestamp;
    if (!GetCurrentTime(&timestamp)) {
        timestamp = svcGetSystemTick();
    }
    
    /* Open report file. */
    snprintf(report_path, sizeof(report_path) - 1, "sdmc:/atmosphere/crash reports/%016lx_%016lx.log", timestamp, process_info.title_id);
    FILE *f_report = fopen(report_path, "w");
    if (f_report == NULL) {
        return;
    }
    
    fprintf(f_report, "Atmosphere Crash Report:\n");
    
    /* TODO: Actually report about the crash. */
    
    fclose(f_report);
}

void CrashReport::BuildReport(u64 pid, bool has_extra_info) {
    this->has_extra_info = has_extra_info;
    if (OpenProcess(pid)) {
        ProcessExceptions();
        if (kernelAbove500()) {
            this->code_list.ReadCodeRegionsFromProcess(this->debug_handle, this->crashed_thread_info.GetPC(), this->crashed_thread_info.GetLR());
            this->thread_list.ReadThreadsFromProcess(this->debug_handle, Is64Bit());
        }
        
        if (IsApplication()) {
            ProcessDyingMessage();
        }
        
        /* Real creport builds the report here. We do it later. */
        
        Close();
    }
}

void CrashReport::ProcessExceptions() {
    if (!IsOpen()) {
        return;
    }
    
    DebugEventInfo d;
    while (R_SUCCEEDED(svcGetDebugEvent((u8 *)&d, this->debug_handle))) {
        switch (d.type) {
            case DebugEventType::AttachProcess:
                HandleAttachProcess(d);
                break;
            case DebugEventType::Exception:
                HandleException(d);
                break;
            case DebugEventType::AttachThread:
            case DebugEventType::ExitProcess:
            case DebugEventType::ExitThread:
            default:
                break;
        }
    }
}

void CrashReport::HandleAttachProcess(DebugEventInfo &d) {
    this->process_info = d.info.attach_process;
    if (kernelAbove500() && IsApplication()) {
        /* Parse out user data. */
        u64 address = this->process_info.user_exception_context_address;
        u64 userdata_address = 0;
        u64 userdata_size = 0;
        
        if (!IsAddressReadable(address, sizeof(userdata_address) + sizeof(userdata_size))) {
            return;
        }
        
        /* Read userdata address. */
        if (R_FAILED(svcReadDebugProcessMemory(&userdata_address, this->debug_handle, address, sizeof(userdata_address)))) {
            return;
        }
        
        /* Validate userdata address. */
        if (userdata_address == 0 || userdata_address & 0xFFF) {
            return;
        }
        
        /* Read userdata size. */
        if (R_FAILED(svcReadDebugProcessMemory(&userdata_size, this->debug_handle, address + sizeof(userdata_address), sizeof(userdata_size)))) {
            return;
        }
        
        /* Cap userdata size. */
        if (userdata_size > sizeof(this->dying_message)) {
            userdata_size = sizeof(this->dying_message);
        }
        
        /* Assign. */
        this->dying_message_address = userdata_address;
        this->dying_message_size = userdata_size;
    }
}

void CrashReport::HandleException(DebugEventInfo &d) {
    this->exception_info = d.info.exception;
    switch (d.info.exception.type) {
        case DebugExceptionType::UndefinedInstruction:
            this->result = (Result)CrashReportResult::UndefinedInstruction;
            break;
        case DebugExceptionType::InstructionAbort:
            this->result = (Result)CrashReportResult::InstructionAbort;
            this->exception_info.specific.raw = 0;
            break;
        case DebugExceptionType::DataAbort:
            this->result = (Result)CrashReportResult::DataAbort;
            break;
        case DebugExceptionType::AlignmentFault:
            this->result = (Result)CrashReportResult::AlignmentFault;
            break;
        case DebugExceptionType::UserBreak:
            this->result = (Result)CrashReportResult::UserBreak;
            /* Try to parse out the user break result. */
            if (kernelAbove500() && IsAddressReadable(this->exception_info.specific.user_break.address, sizeof(this->result))) {
                svcReadDebugProcessMemory(&this->result, this->debug_handle, this->exception_info.specific.user_break.address, sizeof(this->result));
            }
            break;
        case DebugExceptionType::BadSvc:
            this->result = (Result)CrashReportResult::BadSvc;
            break;
        case DebugExceptionType::UnknownNine:
            this->result = (Result)CrashReportResult::UnknownNine;
            this->exception_info.specific.raw = 0;
            break;
        case DebugExceptionType::DebuggerAttached:
        case DebugExceptionType::BreakPoint:
        case DebugExceptionType::DebuggerBreak:
        default:
            return;
    }
    /* Parse crashing thread info. */
    this->crashed_thread_info.ReadFromProcess(this->debug_handle, d.thread_id, Is64Bit());
}

void CrashReport::ProcessDyingMessage() {
    /* Dying message is only stored starting in 5.0.0. */
    if (!kernelAbove500()) {
        return;
    }
    
    /* Validate the message address/size. */
    if (this->dying_message_address == 0 || this->dying_message_address & 0xFFF) {
        return;
    }
    if (this->dying_message_size > sizeof(this->dying_message)) {
        return;
    }
    
    /* Validate that the report isn't garbage. */
    if (!IsOpen() || !WasSuccessful()) {
        return;
    }
    
    if (!IsAddressReadable(this->dying_message_address, this->dying_message_size)) {
        return;
    }
    
    svcReadDebugProcessMemory(this->dying_message, this->debug_handle, this->dying_message_address, this->dying_message_size);
}

bool CrashReport::IsAddressReadable(u64 address, u64 size, MemoryInfo *o_mi) {
    MemoryInfo mi;
    u32 pi;
    
    if (o_mi == NULL) {
        o_mi = &mi;
    }
    
    if (R_FAILED(svcQueryDebugProcessMemory(o_mi, &pi, this->debug_handle, address))) {
        return false;
    }
    
    /* Must be read or read-write */
    if ((o_mi->perm | Perm_W) != Perm_Rw) {
        return false;
    }
    
    /* Must have space for both userdata address and userdata size. */
    if (address < o_mi->addr || o_mi->addr + o_mi->size < address + size) {
        return false;
    }

    return true;
}

bool CrashReport::GetCurrentTime(u64 *out) {
    *out = 0;
    
    /* Verify that pcv isn't dead. */
    {
        Handle dummy;
        if (R_SUCCEEDED(smRegisterService(&dummy, "time:s", false, 0x20))) {
            svcCloseHandle(dummy);
            return false;
        }
    }
    
    /* Try to get the current time. */
    bool success = false;
    if (R_SUCCEEDED(timeInitialize())) {
        if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, out))) {
            success = true;
        }
        timeExit();
    }
    return success;
}
