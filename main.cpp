#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <iostream>
#include <unordered_map>
#include <string>

#include <GLFW/glfw3.h>
#include <lldb/API/LLDB.h>

struct VariableInfo {
    std::string name;
    std::string value;
    bool isLocked = false;
};

std::vector<VariableInfo> variables;

lldb::SBDebugger debugger;
lldb::SBTarget target;
lldb::SBListener listener;
lldb::SBError error;
lldb::SBProcess process;

GLFWwindow* window = nullptr;

void ContinueProcess(lldb::SBProcess& process) {
    if (process.GetState() == lldb::eStateStopped) {
        process.Continue();

        if (process.GetState() != lldb::eStateRunning) {
            std::cerr << "Failed to continue the process." << std::endl;
        }
    } else {
        std::cerr << "Process is not in a stopped state." << std::endl;
    }
}

void FetchAllVariables() {
    variables.clear();

    lldb::SBThread thread = process.GetSelectedThread();
    if (!thread.IsValid()) {
        std::cerr << "No valid thread selected." << std::endl;
        return;
    }

    auto num_frames = thread.GetNumFrames();
    for (auto i = 0; i < num_frames; ++i) {
        lldb::SBFrame frame = thread.GetFrameAtIndex(i);
        if (!frame.IsValid()) {
            std::cerr << "No valid frame selected." << std::endl;
            return;
        }

        lldb::SBValueList frameVariables = frame.GetVariables(true, true, true, true);

        for (int i = 0; i < frameVariables.GetSize(); i++) {
            lldb::SBValue var = frameVariables.GetValueAtIndex(i);
            if (var.IsValid()) {
                bool cont = false;
                for (auto& v : variables) {
                    if (var.GetName() == v.name) {
                        cont = true;
                    }
                }
                if (cont) {
                    continue;
                }

                if (var.GetValue() == nullptr) {
                    continue;
                }
                VariableInfo varInfo;
                varInfo.name = var.GetName();
                varInfo.value = var.GetValue();
                varInfo.isLocked = true;
                variables.push_back(varInfo);
            }
        }
    }
}

void FetchVariableValue(VariableInfo& var_info) {
    process.Stop();

    lldb::SBThread thread = process.GetSelectedThread();
    if (!thread.IsValid()) {
        std::cerr << "Failed to get thread" << std::endl;
        return;
    }

    lldb::SBFrame frame = thread.GetSelectedFrame();
    if (!frame.IsValid()) {
        std::cerr << "Failed to get frame" << std::endl;
        return;
    }

    std::cout << var_info.name << " " << var_info.value << std::endl;
    lldb::SBValue value = frame.EvaluateExpression(var_info.name.c_str());
    if (value.IsValid()) {
        std::cout << "Expression Result: " << value.GetValue() << std::endl;
        var_info.value = std::string(value.GetValue());
        var_info.isLocked = true;
    } else {
        std::cerr << "Failed to evaluate expression" << std::endl;
    }

    ContinueProcess(process);
}

void UpdateVariableValue(VariableInfo& var_info) {
    process.Stop();
    
    lldb::SBThread thread = process.GetSelectedThread();
    if (!thread.IsValid()) {
        std::cerr << "Failed to get thread" << std::endl;
        return;
    }

    bool hard_break = false;
    auto num_frames = thread.GetNumFrames();
    for (auto i = 0; i < num_frames; ++i) {
        lldb::SBFrame frame = thread.GetFrameAtIndex(i);
        if (!frame.IsValid()) {
            std::cerr << "No valid frame selected." << std::endl;
            return;
        }

        lldb::SBValueList frameVariables = frame.GetVariables(true, true, true, true);

        for (int i = 0; i < frameVariables.GetSize(); i++) {
            lldb::SBValue var = frameVariables.GetValueAtIndex(i);
            if (var.IsValid()) {
                if (std::string(var.GetName()) != var_info.name) {
                    continue;
                }
                
                std::string expression = var_info.name + " = " + var_info.value;
                lldb::SBValue value = frame.EvaluateExpression(expression.c_str());
                if (value.IsValid()) {
                    var_info.value = std::string(value.GetValue());
                    hard_break = true;
                    break;
                } else {
                    std::cerr << "Failed to update value of variable " << var_info.name << std::endl;
                }
            }
        }
        if (hard_break) {
            break;
        }
    }

    ContinueProcess(process);
}

void lldb_stuff(const std::string& path, lldb::pid_t pid) {
    lldb::SBDebugger::Initialize();
    debugger = lldb::SBDebugger::Create();
    target = debugger.CreateTargetWithFileAndArch(path.c_str(), nullptr);
    process = target.AttachToProcessWithID(listener, pid, error);

    if (!process.IsValid() || error.Fail()) {
        std::cerr << "Failed to attach to process: " << error.GetCString() << std::endl;
        std::exit(-1);
    }
}

int main(int argc, char** argv) {    
    std::string executable_path = argv[1];
    auto pid = std::atoi(argv[2]);
    lldb_stuff(executable_path, pid);

    // Initialize GLFW
    if (!glfwInit())
        return 1;

    // Decide GL+GLSL versions
#if __APPLE__
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    // glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
#endif

    // Create a windowed mode window and its OpenGL context
    window = glfwCreateWindow(640, 480, "HOOK", NULL, NULL);
    if (window == NULL)
        return 1;

    // Make the window's context current
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    FetchAllVariables();

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create a window with some text fields
        ImGui::Begin("Variables");

        for (auto& var_info : variables) {
            if (!var_info.isLocked) {
                // Allow the user to enter a variable name
                std::string label = "##varname" + var_info.name;
                ImGui::InputText(label.c_str(), &var_info.name);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    std::cout << var_info.name << " " << var_info.value << std::endl;
                    FetchVariableValue(var_info);
                    std::cout << var_info.name << " " << var_info.value << std::endl;
                }
            } else {
                // Display the locked variable name and its value
                ImGui::Text("%s =", var_info.name.c_str());
                ImGui::SameLine();
                std::string label = "##varname" + var_info.name;
                ImGui::InputText(label.c_str(), &var_info.value);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    UpdateVariableValue(var_info);
                }
            }
        }

        ImGui::End();

        // Rendering
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    lldb::SBDebugger::Destroy(debugger);
}