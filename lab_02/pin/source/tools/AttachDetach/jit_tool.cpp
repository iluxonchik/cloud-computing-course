/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2016 Intel Corporation. All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
/*! @file
 * Among other things this test checks:
 *  - KnobCheckOrder- Verify that all image load callbacks and all thread start callbacks are being called before starting to Jit.
 *    (Assuming application do not create new threads after Pin attached to It)
 *
 */

#include "pin.H"
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <assert.h>
#include <set>
#if defined(TARGET_LINUX) || defined(TARGET_ANDROID)
#include <elf.h>
#endif
#include "tool_macros.h"

using namespace std;

/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "o", "jit_tool.out", "specify file name");

KNOB<BOOL> KnobJustInitialNum(KNOB_MODE_WRITEONCE, "pintool",
    "just_init", "0", "just test the initial thread number");

KNOB<BOOL> KnobCheckOrder(KNOB_MODE_WRITEONCE, "pintool",
    "check_order", "0", "Verify that all image load callbacks and all thread start "
            "callbacks are being called before starting to Jit.");

#ifdef TARGET_LINUX
KNOB<BOOL> KnobJustQueryAuxv(KNOB_MODE_WRITEONCE, "pintool",
    "just_auxv", "0", "just test availability of auxv values");
#endif

ofstream TraceFile;
/* ===================================================================== */

volatile BOOL jitBegan = FALSE; // True if Jitting has started

INT32 Usage()
{
    cerr <<
        "This pin tool tests MT attach in JIT mode.\n"
        "\n";
    cerr << KNOB_BASE::StringKnobSummary();
    cerr << endl;
    return -1;
}

#ifdef TARGET_LINUX
void QueryAuxv(const char* name, ADDRINT value)
{
    bool found = false;
    ADDRINT vdso = PIN_GetAuxVectorValue(value, &found);
    if (found)
    {
        TraceFile << name << " value: " << vdso << endl;
    }
    else
    {
        TraceFile << "Could not find auxv value " << name << endl;
    }
}
#endif

UINT32  threadCounter=0;
/*
 * Thread Start callback
 */
PIN_LOCK lock;
VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    ASSERT(!KnobCheckOrder || !jitBegan, "JIT began before all Thread start callbacks were called");
    PIN_GetLock(&lock, PIN_GetTid());
    TraceFile << "Thread counter is updated to " << dec <<  (threadCounter+1) << endl;
    ++threadCounter;
    PIN_ReleaseLock(&lock);
}

/*
 * Sets the value of EAX/RAX to TRUE when We received a notification from all thread
*/

VOID AllThreadsNotifed(unsigned int numOfThreads, ADDRINT* gax)
{
    PIN_GetLock(&lock, PIN_GetTid());
    // Check that we don't have any extra thread
    assert(threadCounter <= numOfThreads);
    assert(*gax == 0);
    if (threadCounter == numOfThreads)
    {
        PIN_ReleaseLock(&lock);
        *gax = 1;
    }
    PIN_ReleaseLock(&lock);
}

int OneThreadNotified()
{
    return threadCounter > 0;
}

void* MyThreadMain(void* arg)
{
    static set<OS_THREAD_ID> tids;
    static set<ADDRINT> args;

    PIN_GetLock(&lock, PIN_GetTid());

    TraceFile << "MyThreadMain called with " << dec << (ADDRINT)arg << endl;
    pair<set<OS_THREAD_ID>::iterator,bool> res = tids.insert(PIN_GetTid());
    ASSERT(res.second, "Thread " + decstr(PIN_GetTid()) + " started twice");
    pair<set<ADDRINT>::iterator,bool> res2 = args.insert((ADDRINT)arg);
    ASSERT(res2.second, "Argument " + decstr((ADDRINT)arg) + " provided twice");

    PIN_ReleaseLock(&lock);
    return arg;
}

int MyPinAttached()
{
    return 1;
}

// Pin calls this function every time a new instruction is encountered
VOID Instruction(INS ins, VOID *v)
{
    jitBegan = TRUE;
}


VOID ImageLoad(IMG img, void *v)
{
    ASSERT(!KnobCheckOrder || !jitBegan, "JIT began before all image load callbacks were called");
    RTN rtn = RTN_FindByName(img, C_MANGLE("ThreadsReady"));
    if (RTN_Valid(rtn))
    {
        // We intentionally use RTN_InsertCall() here (instead of RTN_Replace()).
        // This is because verify_fpstate_app.cpp calls the original function (ThreadsReady())
        // and expects the XMM registers to remain unchanged.
        // Accodring to the ABI, the XMM registers are "scratch registers".
        // This means that the replacement function, AllThreadsNotifed(), might change some of
        // the XMM registers and won't restore them (in fact, this happens on 32 bit OS X*).
        // RTN_Replace() propogates the resulted values of the XMM registers from the replacement function to the
        // application, while RTN_InsertCall() doesn't.
        // So, in this case we choose to use RTN_InsertCall().
        RTN_Open(rtn);
        RTN_InsertCall(rtn, IPOINT_AFTER, AFUNPTR(AllThreadsNotifed), IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_REG_REFERENCE, REG_GAX, IARG_END);
        RTN_Close(rtn);
    }

    rtn = RTN_FindByName(img, C_MANGLE("MainThreadReady"));
    if (RTN_Valid(rtn))
    {
        RTN_Replace(rtn, AFUNPTR(OneThreadNotified));
    }

    rtn = RTN_FindByName(img, C_MANGLE("PinAttached"));
    if (RTN_Valid(rtn))
    {
        RTN_Replace(rtn, AFUNPTR(MyPinAttached));
    }

    rtn = RTN_FindByName(img, C_MANGLE("ThreadMain"));
    if (RTN_Valid(rtn))
    {
        RTN_Replace(rtn, AFUNPTR(MyThreadMain));
    }
}

// This function is called when the application exits
VOID Fini(INT32 code, VOID *v)
{
    // Write to a file since cout and cerr maybe closed by the application
    TraceFile << "Fini was called" << endl;

    TraceFile.close();
}

/* ===================================================================== */

int main(int argc, CHAR *argv[])
{
    PIN_InitSymbols();

    if( PIN_Init(argc,argv) )
    {
        return Usage();
    }

    PIN_InitLock(&lock);

    TraceFile.open(KnobOutputFile.Value().c_str());
    TraceFile << hex;
    TraceFile.setf(ios::showbase);

    if (KnobJustInitialNum) {
        TraceFile << "Initial thread counter: " << PIN_GetInitialThreadCount() << endl;
        TraceFile.close();
        PIN_ExitProcess(0);
    }

#ifdef TARGET_LINUX
    if (KnobJustQueryAuxv) {
        QueryAuxv("AT_ENTRY", AT_ENTRY);
        QueryAuxv("UNDEFINED_ENTRY", 0xFFFFFFF);
        TraceFile.close();
        PIN_ExitProcess(0);
    }
#endif

    if (KnobCheckOrder)
    {
        INS_AddInstrumentFunction(Instruction, 0);
    }
    IMG_AddInstrumentFunction(ImageLoad, 0);
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();

    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
