#include <iostream>
#include <lldb/API/LLDB.h>

lldb::SBDebugger debugger;

void lldb_stuff(const std::string& path, lldb::pid_t pid) {
    lldb::SBDebugger::Initialize();
    debugger = lldb::SBDebugger::Create();

    lldb::SBTarget target = debugger.CreateTargetWithFileAndArch(path.c_str(), nullptr);

    lldb::SBListener listener;
    lldb::SBError error;    
    lldb::SBProcess process = target.AttachToProcessWithID(listener, pid, error);

    // Check if the process is successfully attached
    if (!process.IsValid() || error.Fail()) {
        std::cerr << "Failed to attach to process: " << error.GetCString() << std::endl;
        // Handle error
    }

    //process.Stop();

    lldb::SBThread thread = process.GetSelectedThread();
    if (!thread.IsValid()) {
        // TODO handle
        std::cerr << "Failed to get thread" << std::endl;
        lldb::SBDebugger::Destroy(debugger);
        return;
    }

    lldb::SBFrame frame = thread.GetSelectedFrame();
    if (!frame.IsValid()) {
        std::cerr << "Failed to get frame" << std::endl;
        lldb::SBDebugger::Destroy(debugger);
        return;
    }

    lldb::SBValue value = frame.EvaluateExpression("stop");
    if (value.IsValid()) {
        std::cout << "Expression Result: " << value.GetValue() << std::endl;
    } else {
        std::cerr << "Failed to evaluate expression" << std::endl;
    }

    value = frame.EvaluateExpression("stop = true");
    if (value.IsValid()) {
        std::cout << "Expression Result: " << value.GetValue() << std::endl;
    } else {
        std::cerr << "Failed to evaluate expression" << std::endl;
    }

    //process.Continue();
}

int main(int argc, char** argv) {
    std::string executable_path = argv[1];
    auto pid = std::atoi(argv[2]);
    lldb_stuff(executable_path, pid);
    while (true) {}
    lldb::SBDebugger::Destroy(debugger);
}
