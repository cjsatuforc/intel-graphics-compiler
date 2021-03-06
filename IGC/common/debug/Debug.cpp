/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/
#include "common/debug/Debug.hpp"

#include "common/debug/TeeOutputStream.hpp"

#include "AdaptorCommon/customApi.hpp"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/SourceMgr.h>
#include "common/LLVMWarningsPop.hpp"

#if defined( _WIN32 ) || defined( _WIN64 )
#   include <io.h>
#   include <windows.h>
// Windows.h defines MemoryFence as _mm_mfence, but this conflicts with llvm::sys::MemoryFence
#undef MemoryFence
#endif
#include <sstream>
#include <string>
#include <exception>
#include <stdexcept>
#include <mutex>
using namespace llvm;

#if defined _WIN32 || _WIN64
//#if defined( _DEBUG )
#include "psapi.h"
int CatchAssert( int reportType, char *userMessage, int *retVal )
{
    if(IsDebuggerPresent())
    {
        *retVal = 1; // Break into the debugger or print a stack
    }
    else
    {
        *retVal = 0;
    }
    return true; // we always want to abort, return false pops up a window
}

BOOL WINAPI DllMain(
    _In_  HINSTANCE hinstDLL,
    _In_  DWORD fdwReason,
    _In_  LPVOID lpvReserved
    )
{
    TCHAR path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    //_splitpath_s((LPCWSTR *)path, NULL, 0, NULL, 0, (LPCWSTR *)name, MAX_PATH, NULL, 0);
    // temporary force the crash for both standalone and driver
    //if(!_stricmp("IGCStandalone", name) || !_stricmp("IGCStandalone64", name))
    {
        _CrtSetReportHook(CatchAssert);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDOUT);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDOUT);
        _set_error_mode(_OUT_TO_STDERR);
    }
    switch (fdwReason) {
    case DLL_PROCESS_DETACH:
        llvm_shutdown();
        break;
        
    case DLL_PROCESS_ATTACH:
        break;

    case DLL_THREAD_DETACH:
        break;

    case DLL_THREAD_ATTACH:
        break;
    }
    return TRUE;
}
#endif

namespace IGC
{
namespace Debug
{

#if defined _WIN32 || WIN64
namespace
{
    class DebugOutputStream
        : public raw_ostream
    {
    public:
        DebugOutputStream()
            : raw_ostream( true /* unbuffered */ )
        {
        }

        ~DebugOutputStream()
        {
            flush();
        }

        bool has_colors()
        {
            return false;
        }

        bool is_displayed()
        {
            return true;
        }

        virtual uint64_t current_pos() const
        {
            return 0;
        }

        size_t preferred_buffer_size() const
        {
            return 0;
        }

    private:
        virtual void write_impl(const char* Ptr, size_t Size)
        {
            std::stringstream ss;
            ss.write(Ptr, Size);
            OutputDebugStringA(ss.str().c_str());
        }
    };
} // end anonymous namespace
#endif

void Banner(raw_ostream & OS, std::string const& message)
{
#if _DEBUG
    if ( GetDebugFlag(DebugFlag::DUMPS) )
    {
        OS.changeColor( raw_ostream::YELLOW, true, false );
        OS << "-----------------------------------------\n";
        OS << message << "\n";
        OS << "-----------------------------------------\n";
        OS.resetColor();
    }
#endif
}

raw_ostream &ods()
{
#if defined( _DEBUG ) || defined( _INTERNAL )
    if ( IGC_IS_FLAG_ENABLED(PrintToConsole) )
    {
#    if defined _WIN32 || WIN64
        {
            static DebugOutputStream dos;
            static TeeOutputStream tee( errs(), dos );
            if ( GetDebugFlag(DebugFlag::DUMP_TO_OUTS) )
            {
                return tee;
            }
            else
            {
                return dos;
            }
        }
#    else
        {
            if ( GetDebugFlag(DebugFlag::DUMP_TO_OUTS) )
            {
                return outs();
            }
            else
            {
                return nulls();
            }
        }
#    endif
    }
    else
#endif
    {
        return nulls();
    }
}


void Warning(
    const char*           pExpr,
    unsigned int          line,
    const char*           pFileName,
    std::string    const& message )
{
#if _DEBUG
    if ( GetDebugFlag(DebugFlag::DUMPS) )
    {
        ods().changeColor( raw_ostream::MAGENTA, true, false );
        ods() << pFileName << "(" << line << ") : ";
        ods() << "IGC Warning: (" << pExpr << ") " << message << "\n";
        ods().resetColor();
        ods().flush();
    }
#endif
}

namespace {
    void FatalErrorHandler(void *user_data, const std::string& reason, bool gen_crash_diag)
    {
        (void)user_data;
        (void)reason;
#if defined( _DEBUG )
#if defined( _WIN32 )
        OutputDebugStringA("LLVM Error: ");
        OutputDebugStringA(reason.c_str());
        OutputDebugStringA("\n");
#endif
        fprintf( stderr, "%s", "LLVM Error: " );
        fprintf( stderr, "%s", reason.c_str());
        fprintf( stderr, "%s", "\n");
        fflush( stderr );

        if ( reason != "IO failure on output stream." )
        {
            exit(1);
        }
#endif
    }

    void ComputeFatalErrorHandler(const DiagnosticInfo &DI, void * /*Context*/) {
        // Skip non-error diag.
        if (DI.getSeverity() != DS_Error)
            return;
        std::string MsgStorage;
        raw_string_ostream Stream(MsgStorage);
        DiagnosticPrinterRawOStream DP(Stream);
        DI.print(DP);
        Stream.flush();

        std::string msg;
        msg += "\nerror: ";
        msg += MsgStorage;
        msg += "\nerror: backend compiler failed build.\n";
    }
}

void RegisterErrHandlers()
{
    static bool executed = false;
    if (!executed)
    {
        install_fatal_error_handler( FatalErrorHandler, nullptr );
        executed = true;
    }
}

void RegisterComputeErrHandlers(LLVMContext &C)
{
    C.setDiagnosticHandler(ComputeFatalErrorHandler);
}

void ReleaseErrHandlers()
{
    // do nothing
}

static std::mutex stream_mutex;

void DumpLock()
{
    stream_mutex.lock();
}

void DumpUnlock()
{
    stream_mutex.unlock();
}

} // namespace Debug

int getPointerSize(llvm::Module &M) {
    return M.getDataLayout().getPointerSize();
}

} // namespace IGC
