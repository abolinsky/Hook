#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <iostream>
#include <unordered_map>
#include <string>
#include <sstream>

#include <GLFW/glfw3.h>
#include <lldb/API/LLDB.h>

struct VariableInfo {
    std::string name;
    std::string value;
    bool isLocked = false;
    bool isStruct = false;
    std::vector<VariableInfo> children;
};

std::vector<VariableInfo> variables;
VariableInfo current_var_info;

lldb::SBDebugger debugger;
lldb::SBTarget target;
lldb::SBListener listener;
lldb::SBError error;
lldb::SBProcess process;

GLFWwindow* window = nullptr;

void FetchVariableMembers(lldb::SBValue& value, std::vector<VariableInfo>& variables, const std::string& parentName = "") {
    if (!value.IsValid()) {
        return;
    }

    lldb::SBType valueType = value.GetType();
    lldb::TypeClass typeClass = valueType.GetTypeClass();

    if (typeClass == lldb::eTypeClassStruct || typeClass == lldb::eTypeClassClass) {
        const int numChildren = value.GetNumChildren();
        for (int i = 0; i < numChildren; ++i) {
            lldb::SBValue child = value.GetChildAtIndex(i);
            if (child.IsValid()) {
                std::string childName = parentName.empty() ? value.GetName() : parentName + "." + value.GetName();
                FetchVariableMembers(child, variables, childName);
            }
        }
    } else {
        if (!value.GetValue()) {
            return;
        }

        VariableInfo varInfo;
        varInfo.name = parentName.empty() ? value.GetName() : parentName + "." + value.GetName();
        varInfo.value = value.GetValue();
        varInfo.isLocked = true;
        variables.push_back(varInfo);
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
            FetchVariableMembers(var, variables);
        }
    }
}

std::vector<std::string> split(const std::string &s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);

    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}

void UpdateVariableValue(VariableInfo& var_info) {
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

        auto names = split(var_info.name, '.');
        lldb::SBValue var = frame.FindVariable(names[0].c_str());
        if (var.IsValid()) {
            std::string expression = var_info.name + " = " + var_info.value;
            lldb::SBValue value = frame.EvaluateExpression(expression.c_str());
            if (value.IsValid() && value.GetValue()) {
                var_info.value = std::string(value.GetValue());
                break;
            } else {
                std::cerr << "Failed to update value of variable " << var_info.name << std::endl;
            }
        }
    }
}

void lldb_stuff(const std::string& path, lldb::pid_t pid) {
    lldb::SBDebugger::Initialize();
    debugger = lldb::SBDebugger::Create();
    target = debugger.CreateTarget(path.c_str());
    process = target.AttachToProcessWithID(listener, pid, error);

    if (!process.IsValid() || error.Fail()) {
        std::cerr << "Failed to attach to process: " << error.GetCString() << std::endl;
        std::exit(-1);
    }

    // Set up lldb event listener
    listener = debugger.GetListener();
    process.GetBroadcaster().AddListener(listener, lldb::SBProcess::eBroadcastBitStateChanged);
}

void HandleProcessEvents() {
    lldb::SBEvent event;
    while (listener.PeekAtNextEvent(event)) {
        if (lldb::SBProcess::EventIsProcessEvent(event)) {
            lldb::StateType state = lldb::SBProcess::GetStateFromEvent(event);

            switch (state) {
                case lldb::eStateStopped:
                    UpdateVariableValue(current_var_info);
                    FetchAllVariables();
                    process.Continue();
                    break;
                case lldb::eStateRunning:
                    break;
                case lldb::eStateExited:
                    break;
                default:
                    break;
            }
        }
        listener.GetNextEvent(event);
    }
}

void DisplayVariable(VariableInfo& varInfo) {
    if (varInfo.isStruct) {
        if (ImGui::TreeNode(varInfo.name.c_str())) {
            for (auto& childVar : varInfo.children) {
                DisplayVariable(childVar);
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::Text("%s =", varInfo.name.c_str());
        ImGui::SameLine();
        std::string label = "##varname" + varInfo.name;
        ImGui::InputText(label.c_str(), &varInfo.value);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            current_var_info = varInfo;
            process.Stop();
        }
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
    process.Continue();

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Handle LLDB process events
        HandleProcessEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create a window with some text fields
        ImGui::Begin("Variables");

        for (auto& varInfo : variables) {
            DisplayVariable(varInfo);
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