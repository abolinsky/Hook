#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <limits>

#include <GLFW/glfw3.h>
#include <lldb/API/LLDB.h>


struct VariableInfo {
    std::string function_name;
    std::string name;
    std::string value;
    uint64_t id = std::numeric_limits<uint64_t>::max();
    bool isLocked = false;
    bool isNested = false;
    std::vector<VariableInfo> children;
    VariableInfo* parent = nullptr;
};

std::vector<VariableInfo> variables;
VariableInfo current_var_info;
bool published_changes = true;

constexpr unsigned long long delay = 500;

lldb::SBDebugger debugger;
lldb::SBTarget target;
lldb::SBListener listener;
lldb::SBError error;
lldb::SBProcess process;

GLFWwindow* window = nullptr;


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
            continue;
        }

        std::string full_qualified_name = var_info.name;
        const VariableInfo* root = &var_info;
        while (true) {
            if (root->parent) {
                root = root->parent;
                full_qualified_name = root->name + "." + full_qualified_name;
            } else {
                break;
            }
        }

        lldb::SBValueList vars = frame.GetVariables(true, true, true, true);
        lldb::SBValue var;
        for (int j = 0; j < vars.GetSize(); ++j) {
            auto v = vars.GetValueAtIndex(j);
            if (v.GetID() == root->id) {
                var = v;
                break;
            }
        }
        if (!var.IsValid()) {
            continue;
        }
        
        std::string expression = full_qualified_name + " = " + var_info.value;
        lldb::SBValue value = frame.EvaluateExpression(expression.c_str());
        if (!value.IsValid() || !value.GetValue()) {
            std::cerr << "Failed to update value of variable " << var_info.name << std::endl;
            return;
        }
    }
}

void DisplayVariable(const VariableInfo& varInfo) {
    if (varInfo.isNested) {
        if (varInfo.parent) {
            ImGui::Text("%s =", varInfo.name.c_str());
        } else {
            if (varInfo.function_name.empty()) {
                ImGui::Text("%s =", varInfo.name.c_str());
            } else {
                ImGui::Text("(%s) %s =", varInfo.function_name.c_str(), varInfo.name.c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::TreeNode((std::string("##varname") + std::to_string(varInfo.id)).c_str())) {
            for (const auto& childVar : varInfo.children) {
                DisplayVariable(childVar);
            }
            ImGui::TreePop();
        }
    } else {
        if (!varInfo.parent) {
            if (varInfo.function_name.empty()) {
                ImGui::Text("%s =", varInfo.name.c_str());
            } else {
                ImGui::Text("(%s) %s =", varInfo.function_name.c_str(), varInfo.name.c_str());
            }
        } else {
            ImGui::Text("%s =", varInfo.name.c_str());
        }
        
        ImGui::SameLine();
        std::string label = "##varname" + varInfo.function_name + varInfo.name;

        ImGui::InputText(label.c_str(), const_cast<std::string*>(&varInfo.value));

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            current_var_info = varInfo;
            published_changes = false;
            process.Stop();
        }
    }
}

void FetchStructMembers(lldb::SBValue& structValue, VariableInfo& parent) {
    for (int i = 0; i < structValue.GetNumChildren(); ++i) {
        lldb::SBValue childValue = structValue.GetChildAtIndex(i);
        if (childValue.IsValid()) {
            lldb::TypeClass typeClass = childValue.GetType().GetTypeClass();
            bool isStruct = typeClass == lldb::eTypeClassStruct;
            bool isClass = typeClass == lldb::eTypeClassClass;
            bool childIsNested = isStruct || isClass;

            VariableInfo childVarInfo {
                .function_name = parent.function_name,
                .name = childValue.GetName(),
                .value = childValue.GetValue() ? childValue.GetValue() : "",
                .id = childValue.GetID(),
                .isLocked = true,
                .isNested = childIsNested,
                .parent = &parent
            };

            parent.children.push_back(childVarInfo);

            if (childIsNested) {
                FetchStructMembers(childValue, parent.children.back());
            }

        }
    }
}

void FetchVariablesFromFrame(lldb::SBFrame& frame, std::vector<VariableInfo>& variables) {
    lldb::SBValueList frameVariables = frame.GetVariables(true, true, true, true); // arguments, locals, statics, in_scope_only

    for (int i = 0; i < frameVariables.GetSize(); i++) {
        lldb::SBValue var = frameVariables.GetValueAtIndex(i);
        if (var.IsValid()) {
            lldb::TypeClass typeClass = var.GetType().GetTypeClass();
            bool isStruct = typeClass == lldb::eTypeClassStruct;
            bool isClass = typeClass == lldb::eTypeClassClass;
            bool isNested = isStruct || isClass;
            
            VariableInfo varInfo {
                .function_name = frame.GetFunction().GetName(),
                .name = var.GetName(),
                .value = var.GetValue() ? var.GetValue() : "",
                .id = var.GetID(),
                .isLocked = true,
                .isNested = isNested
            };

            if (varInfo.name.substr(0,2) == "::") {
                varInfo.function_name = "";
            } else {
                varInfo.function_name = frame.GetFunctionName();
            }

            bool already_exists = false;
            for (const auto& var : variables) {
                if (var.function_name == varInfo.function_name && var.name == varInfo.name) {
                    already_exists = true;
                    break;
                }
            }

            if (!already_exists) {
                variables.push_back(varInfo);
            }

            if (isNested) {
                FetchStructMembers(var, variables.back());
            }

        }
    }
}

void FetchAllVariables() {
    variables.clear();
    variables.reserve(500); // FIXME

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

        FetchVariablesFromFrame(frame, variables);
    }
}

void HandleLLDBProcessEvents() {
    lldb::SBEvent event;
    while (listener.PeekAtNextEvent(event)) {
        if (lldb::SBProcess::EventIsProcessEvent(event)) {
            lldb::StateType state = lldb::SBProcess::GetStateFromEvent(event);

            switch (state) {
                case lldb::eStateStopped:
                    if (!published_changes) {
                        UpdateVariableValue(current_var_info);
                        published_changes = true;
                    }
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

void MainLoop() {
    FetchAllVariables();
    process.Continue();

    unsigned long long delay_tick = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        HandleLLDBProcessEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create a window with some text fields
        ImGui::Begin("Variables");
        for (const auto& var : variables) {
            DisplayVariable(var);
        }

        ImGui::End();

        // Rendering
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        ++delay_tick % delay == 0 && process.Stop();
    }
}

void TearDownGraphics() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
}

bool SetupGraphics() {
    if (!glfwInit()) return false;

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

    window = glfwCreateWindow(640, 480, "hook", NULL, NULL);
    if (window == NULL) return false;

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    return true;
}

bool SetupEventListener() {
    listener = debugger.GetListener();
    auto event_mask = lldb::SBProcess::eBroadcastBitStateChanged;
    auto event_bits = process.GetBroadcaster().AddListener(listener, event_mask);
    return event_bits == event_mask;
}

bool AttachToProcess(lldb::SBAttachInfo& attachInfo) {
    target = debugger.CreateTarget("");
    process = target.Attach(attachInfo, error);
    if (!process.IsValid() || error.Fail()) {
        std::cerr << "Failed to attach to process: " << error.GetCString() << std::endl;
        return false;
    }
    return true;
}

bool AttachToProcessWithID(lldb::pid_t pid) {
    lldb::SBAttachInfo attachInfo;
    attachInfo.SetProcessID(pid);
    return AttachToProcess(attachInfo);
}

void TearDownDebugger() {
    lldb::SBDebugger::Destroy(debugger);
}

bool SetupDebugger(const std::string& process_string) {
    lldb::SBDebugger::Initialize();
    debugger = lldb::SBDebugger::Create();

    try {
        lldb::pid_t pid = std::stol(process_string);
        return AttachToProcessWithID(pid);
    } catch (...) {
        return false;
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <pid>" << std::endl;
        return -1;
    }

    std::string process_string = argv[1];

    if (!SetupDebugger(process_string)) return -1;
    if (!SetupEventListener()) return -1;
    if (!SetupGraphics()) return -1;

    MainLoop();

    TearDownGraphics();
    TearDownDebugger();
}