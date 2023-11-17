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
    VariableInfo(lldb::SBValue& value) {
        name = value.GetName();
        function_name = (name.substr(0,2) == "::") ? "" : value.GetFrame().GetFunctionName();
        this->value = value.GetValue() ? value.GetValue() : "";
        type_name = value.GetTypeName();
        id = value.GetID();

        lldb::TypeClass typeClass = value.GetType().GetTypeClass();
        isNested = typeClass == lldb::eTypeClassStruct || typeClass == lldb::eTypeClassClass;
    }

    VariableInfo() = default;

    const VariableInfo& GetRoot() const {
        const VariableInfo* root = this;
        while (root->parent) {
            root = root->parent;
        }
        return *root;
    }

    std::string GetFullyQualifiedName() const {
        std::string full_name = name;
        const VariableInfo* root = this;
        while (root->parent) {
            root = root->parent;
            full_name = root->name + "." + full_name;
        }
        return full_name;
    }

    std::string name;
    std::string function_name;
    std::string value;
    std::string type_name;
    uint64_t id = std::numeric_limits<uint64_t>::max();
    bool isNested = false;
    std::vector<VariableInfo*> children;
    VariableInfo* parent = nullptr;
};

std::vector<VariableInfo> variables;
const VariableInfo* current_var_info;
bool published_changes = true;

lldb::SBDebugger debugger;
lldb::SBTarget target;
lldb::SBListener listener;
lldb::SBError error;
lldb::SBProcess process;

GLFWwindow* window = nullptr;

void PublishChange(const VariableInfo& varInfo) {
    current_var_info = &varInfo;
    published_changes = false;
    process.Stop();
}

void DisplayVariable(const VariableInfo& varInfo) {
    std::string prefix = varInfo.parent ? "" : varInfo.function_name.empty() ? "" : "(" + varInfo.function_name + ") ";
    ImGui::Text("%s%s =", prefix.c_str(), varInfo.name.c_str());
    ImGui::SameLine();

    if (varInfo.isNested) {
        std::string treeNodeLabel = "##varname" + std::to_string(varInfo.id);
        if (ImGui::TreeNode(treeNodeLabel.c_str())) {
            for (auto childVar : varInfo.children) {
                DisplayVariable(*childVar);
            }
            ImGui::TreePop();
        }
    } else {
        std::string inputTextLabel = "##varname" + varInfo.function_name + varInfo.name;
        ImGui::InputText(inputTextLabel.c_str(), const_cast<std::string*>(&varInfo.value));

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            PublishChange(varInfo);
        }
    }
}

void Draw() {
    ImGui::Begin("Variables");
    for (auto& var : variables) {
        if (!var.parent) {
            DisplayVariable(var);
        }
    }
}

lldb::SBValue FindVariableById(lldb::SBFrame& frame, uint64_t id) {
    lldb::SBValueList vars = frame.GetVariables(true, true, true, true);
    for (int i = 0; i < vars.GetSize(); ++i) {
        lldb::SBValue v = vars.GetValueAtIndex(i);
        if (v.GetID() == id) {
            return v;
        }
    }
    return lldb::SBValue();
}

std::vector<lldb::SBValue> GetVariablesFromFrame(lldb::SBFrame& frame) {
    std::vector<lldb::SBValue> variables;

    lldb::SBValueList frameVariables = frame.GetVariables(true, true, true, true); // arguments, locals, statics, in_scope_only
    for (int i = 0; i < frameVariables.GetSize(); i++) {
        lldb::SBValue var = frameVariables.GetValueAtIndex(i);
        if (var) {
            variables.push_back(var);
        }
    }
    return variables;
}

lldb::SBThread GetThread(lldb::SBProcess& process) {
    lldb::SBThread thread = process.GetSelectedThread();
    if (!thread) {
        std::cerr << "Failed to get thread" << std::endl;
    }
    return thread;
}

std::vector<lldb::SBFrame> GetFrames(lldb::SBThread& thread) {
    std::vector<lldb::SBFrame> frames;
    const auto num_frames = thread.GetNumFrames();
    frames.reserve(num_frames);

    for (auto i = 0; i < num_frames; ++i) {
        lldb::SBFrame frame = thread.GetFrameAtIndex(i);
        if (frame) {
            frames.push_back(frame);
        }
    }
    return frames;
}

void UpdateVariableValue(const VariableInfo* var_info) {
    auto thread = GetThread(process);
    if (!thread) return;

    std::string fully_qualified_name = var_info->GetFullyQualifiedName();
    std::string expression = fully_qualified_name + " = " + var_info->value;

    for (auto& frame : GetFrames(thread)) {
        lldb::SBValue var = FindVariableById(frame, var_info->GetRoot().id);
        if (!var) continue;

        lldb::SBValue value = frame.EvaluateExpression(expression.c_str());
        if (value && value.GetValue()) return;
    }

    std::cerr << "Failed to update value of " << var_info->GetFullyQualifiedName() << std::endl;
}

void FetchNestedMembers(lldb::SBValue& structValue, VariableInfo& parent) {
    for (int i = 0; i < structValue.GetNumChildren(); ++i) {
        lldb::SBValue childValue = structValue.GetChildAtIndex(i);
        if (!childValue.IsValid()) continue;

        variables.emplace_back(childValue);
        variables.back().parent = &parent;
        parent.children.push_back(&variables.back());

        if (parent.children.back()->isNested) {
            FetchNestedMembers(childValue, *parent.children.back());
        }
    }
}

std::vector<lldb::SBValue> GetVariablesFromThread(lldb::SBThread& thread) {
    std::vector<lldb::SBValue> variables;
    for (auto& frame : GetFrames(thread)) {
        auto frame_vars = GetVariablesFromFrame(frame);
        variables.insert(variables.end(), frame_vars.begin(), frame_vars.end());
    }
    return variables;
}

void FetchAllVariables() {
    auto thread = GetThread(process);
    if (!thread) return;

    auto thread_variables = GetVariablesFromThread(thread);
    
    variables.clear();
    variables.reserve(thread_variables.size() * 100);

    for (auto& var : thread_variables) {
        const VariableInfo varInfo(var);

        if (std::none_of(variables.begin(), variables.end(), [&varInfo](const VariableInfo& v) {
            return v.function_name == varInfo.function_name && v.name == varInfo.name;
        })) {
            variables.push_back(varInfo);
            if (varInfo.isNested) {
                FetchNestedMembers(var, variables.back());
            }
        }
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

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        HandleLLDBProcessEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        Draw();

        ImGui::End();

        // Rendering
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
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