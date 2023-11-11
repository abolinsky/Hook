#include <iostream>
#include <stdexcept>
#include <sstream>

#include <unistd.h>

#include <lldb/API/LLDB.h>

int fd[2];

struct ProcessData {
    uintptr_t address = 0;
    uintptr_t size = 0;
    uintptr_t data = 0;
    unsigned int dataCnt = 0;
};

void lldb_stuff(const std::string& path, lldb::pid_t pid, ProcessData& data) {
    lldb::SBDebugger::Initialize();
    lldb::SBDebugger debugger = lldb::SBDebugger::Create();

    lldb::SBTarget target = debugger.CreateTargetWithFileAndArch(path.c_str(), nullptr);

    lldb::SBListener listener;
    lldb::SBError error;    
    lldb::SBProcess process = target.AttachToProcessWithID(listener, pid, error);

    // Check if the process is successfully attached
    if (!process.IsValid() || error.Fail()) {
        std::cerr << "Failed to attach to process: " << error.GetCString() << std::endl;
        // Handle error
    }

    // Reading memory
    lldb::addr_t address = data.address;
    size_t size = data.size;
    char buffer[size];
    process.ReadMemory(address, buffer, size, error);

    // Check for read error
    if (error.Fail()) {
        std::cerr << "Memory read error: " << error.GetCString() << std::endl;
        // Handle error
    }

    buffer[0] = 1;
    process.WriteMemory(address, buffer, size, error);

    // Check for write error
    if (error.Fail()) {
        std::cerr << "Memory write error: " << error.GetCString() << std::endl;
        // Handle error
    }

    // Clean up
    lldb::SBDebugger::Destroy(debugger);
}

pid_t launch(const std::string& path) {
    if (pipe(fd) == -1) {
        std::cerr << "Pipe failed" << std::endl;
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        return -1;
    } else if (pid > 0) {
        // Parent process
        close(fd[1]); // Close unused write end
        
        std::cout << "Started process with PID: " << pid << std::endl;
        return pid;
    } else {
        // Child process
        close(fd[0]); // Close unused read end
        dup2(fd[1], STDOUT_FILENO); // Redirect stdout to the pipe
        close(fd[1]);

        execl(path.c_str(), "tracee-program", (char *)NULL);

        // execl only returns if there is an error
        std::cerr << "Failed to start " << path << std::endl;
        return -2;
    }
}

ProcessData receive_tracee_data() {
    ProcessData data;

    char buffer[128];
    ssize_t count;
    std::string receivedData;
    while ((count = read(fd[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[count] = '\0';
        receivedData += buffer;
        break;
    }

    if (count == 0) {
        return data;
    }

    std::cout << "Received from tracee: " << receivedData << std::endl;

    std::istringstream iss(receivedData);
    std::string addressStr, typeInfo;
    if (iss >> addressStr >> typeInfo) {
        // Convert address string to vm_address_t
        std::istringstream addressStream(addressStr);
        addressStream >> std::hex >> data.address;

        // Determine the size based on typeInfo
        if (typeInfo == "b") {  // Boolean type
            data.size = sizeof(bool);
        }
        // Add more type checks if necessary
    }

    return data;
}

int main(int argc, char** argv) {

    std::string executable_path = argv[1];
    auto pid = launch(executable_path);

    while (true) {
        auto data = receive_tracee_data();
        if (data.address) {
            lldb_stuff(executable_path, pid, data);
        }
    }
}
