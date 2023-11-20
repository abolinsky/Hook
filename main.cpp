#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>
#include <lldb/API/LLDB.h>

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <limits>


struct VariableInfo {
    VariableInfo(lldb::SBValue& value) {
        this->name = value.GetName();
        this->function_name = (name.substr(0,2) == "::") ? "" : value.GetFrame().GetFunctionName();
        this->id = value.GetID();

        this->type = value.GetType().GetCanonicalType();
        while (type.GetTypeClass() == lldb::eTypeClassTypedef) {
            this->type.GetTypedefedType();
        }

        this->basic_type = this->type.GetBasicType();
        if (this->basic_type == lldb::eBasicTypeUnsignedChar) {
            value.SetFormat(lldb::Format::eFormatDecimal);
        }

        this->is_nested = this->type.IsAggregateType();
        if (!this->is_nested) {
            this->value = value.GetValue();
        }
    }

    VariableInfo() = default;

    bool IsAggregateType() const {
        return is_nested;
    }

    const VariableInfo& GetRoot() const {
        const VariableInfo* root = this;
        while (root->parent) {
            root = root->parent;
        }
        return *root;
    }

    bool IsRoot() const {
        return !parent;
    }

    bool ParentIsContainer() const {
        return name.back() == ']';
    }

    std::string GetFullyQualifiedName() const {
        std::string full_name;
        const VariableInfo* current = this;
        while (current != nullptr) {
            full_name = current->name + full_name;
            if (!current->IsRoot() && !current->ParentIsContainer()) {
                full_name = "." + full_name;
            }
            current = current->parent;
        }
        return full_name;
    }

    std::string name;
    std::string function_name;
    std::string value;
    lldb::SBType type;
    lldb::BasicType basic_type;
    bool is_nested = false;
    uint64_t id = std::numeric_limits<uint64_t>::max();
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

lldb::pid_t pid = 0;

GLFWwindow* window = nullptr;

void PublishChange(const VariableInfo& varInfo) {
    current_var_info = &varInfo;
    published_changes = false;
    process.Stop();
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

void DisplayVariable(const VariableInfo& varInfo) {
    std::string prefix = !varInfo.IsRoot() ? "" : varInfo.function_name.empty() ? "" : "(" + varInfo.function_name + ") ";
    ImGui::Text("%s%s =", prefix.c_str(), varInfo.name.c_str());
    ImGui::SameLine();

    if (varInfo.IsAggregateType()) {
        std::string treeNodeLabel = "##varname" + std::to_string(varInfo.id);
        if (ImGui::TreeNode(treeNodeLabel.c_str())) {
            for (auto childVar : varInfo.children) {
                DisplayVariable(*childVar);
            }
            ImGui::TreePop();
        }
    } else {
        std::string inputTextLabel = "##varname" + varInfo.function_name + varInfo.name;
        if (varInfo.basic_type == lldb::eBasicTypeBool) {
            bool value = varInfo.value == "true";
            ImGui::Checkbox(inputTextLabel.c_str(), &value);
            const_cast<std::string&>(varInfo.value) = value ? "true" : "false";
        } else if (varInfo.basic_type == lldb::eBasicTypeUnsignedChar) {
            uint8_t value = std::stoul(varInfo.value);
            int value_int = value;
            ImGui::SliderInt(inputTextLabel.c_str(), &value_int, std::numeric_limits<uint8_t>::min(), std::numeric_limits<uint8_t>::max());
            const_cast<std::string&>(varInfo.value) = std::to_string(value_int);
        } else {
            ImGui::InputText(inputTextLabel.c_str(), const_cast<std::string*>(&varInfo.value));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            PublishChange(varInfo);
        }
    }
}

void FetchNestedMembers(lldb::SBValue& aggregateValue, VariableInfo& parent) {
    for (int i = 0; i < aggregateValue.GetNumChildren(); ++i) {
        lldb::SBValue childValue = aggregateValue.GetChildAtIndex(i);
        if (!childValue.IsValid()) continue;

        variables.emplace_back(childValue);
        variables.back().parent = &parent;
        parent.children.push_back(&variables.back());

        if (parent.children.back()->IsAggregateType()) {
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
            if (varInfo.IsAggregateType()) {
                FetchNestedMembers(var, variables.back());
            }
        }
    }
}

void AttachToProcess(lldb::SBAttachInfo& attachInfo) {
    target = debugger.CreateTarget("");
    process = target.Attach(attachInfo, error);
    if (!process.IsValid() || error.Fail()) {
        throw std::runtime_error(std::string("Failed to attach to process: ") + error.GetCString());
    }
}

void AttachToProcessWithID(lldb::pid_t pid) {
    lldb::SBAttachInfo attachInfo;
    attachInfo.SetProcessID(pid);
    AttachToProcess(attachInfo);
}

void SetupEventListener() {
    listener = debugger.GetListener();
    auto event_mask = lldb::SBProcess::eBroadcastBitStateChanged;
    auto event_bits = process.GetBroadcaster().AddListener(listener, event_mask);
    if (event_bits != event_mask) {
        throw std::runtime_error("Could not set up event listener");
    }
}

void HandleAttachProcess() {
    AttachToProcessWithID(pid);
    SetupEventListener();
    FetchAllVariables();
    process.Continue();
}

void Draw() {
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowPos(ImVec2(0, 0));

    if (!ImGui::Begin("Variables", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::End();
        return;
    }
    
    bool open_pid_popup = false;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Attach with PID")) {
                open_pid_popup = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    
    if (ImGui::BeginPopup("AttachWithPID", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove)) {
        static bool attach_failed = false;

        std::string pidInput;
        pidInput.reserve(64);
        ImGui::Text("PID:");
        ImGui::SameLine();
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##pid", &pidInput, ImGuiInputTextFlags_EnterReturnsTrue)) {
            pid = std::stoull(pidInput);
            try {
                HandleAttachProcess();
                ImGui::CloseCurrentPopup();
                attach_failed = false;
            } catch (...) {
                attach_failed = true;
            }
        }
        if (attach_failed) {
            ImGui::TextDisabled("(!) Could not attach to pid %llu", pid);
        }
        ImGui::EndPopup();
    }

    if (open_pid_popup) {
        open_pid_popup = false;
        ImGui::OpenPopup("AttachWithPID");
    }

    for (auto& var : variables) {
        if (var.IsRoot()) {
            DisplayVariable(var);
        }
    }

    ImGui::End();
    ImGui::Render();
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

void HandleLLDBProcessEvents() {
    lldb::SBEvent event;
    while (listener.PeekAtNextEvent(event)) {
        if (lldb::SBProcess::EventIsProcessEvent(event)) {
            lldb::StateType state = lldb::SBProcess::GetStateFromEvent(event);

            if (state == lldb::eStateStopped) {
                if (!published_changes) {
                    UpdateVariableValue(current_var_info);
                    published_changes = true;
                }
                FetchAllVariables();
                process.Continue();
            }
        }
        listener.GetNextEvent(event);
    }
}

void FrontendPreDraw() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
}

void FrontendPostDraw() {
    ImGui::End();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void BackendPreDraw() {
    glfwPollEvents();
}

void BackendPostDraw() {
    glfwSwapBuffers(window);
}

bool ShouldRemainOpen() {
    return !glfwWindowShouldClose(window);
}

void MainLoop() {
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        HandleLLDBProcessEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();

        Draw();

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }
}

void SetStyles() {
    auto yellow = ImVec4{0.996, 0.780, 0.008, 1.0};
    auto blue = ImVec4{0.090, 0.729, 0.808, 1.0};
    auto green = ImVec4{0.149, 0.918, 0.694, 1.0};
    auto white = ImVec4{0.996, 0.996, 0.996, 1.0};
    auto red = ImVec4{1.000, 0.353, 0.322, 1.0};
    auto dark_gray = ImVec4{0.1f, 0.1f, 0.13f, 1.0};
    auto middle_gray = ImVec4{0.5f, 0.5f, 0.5f, 1.0};

    auto &colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg] = dark_gray;
    colors[ImGuiCol_MenuBarBg] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

    // Border
    colors[ImGuiCol_Border] = ImVec4{0.44f, 0.37f, 0.61f, 0.29f};
    colors[ImGuiCol_BorderShadow] = ImVec4{0.0f, 0.0f, 0.0f, 0.24f};

    // Text
    colors[ImGuiCol_Text] = ImVec4{1.0f, 1.0f, 1.0f, 1.0f};
    colors[ImGuiCol_TextDisabled] = ImVec4{0.5f, 0.5f, 0.5f, 1.0f};

    // Headers
    colors[ImGuiCol_Header] = ImVec4{0.13f, 0.13f, 0.17, 1.0f};
    colors[ImGuiCol_HeaderHovered] = ImVec4{0.19f, 0.2f, 0.25f, 1.0f};
    colors[ImGuiCol_HeaderActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

    // Buttons
    colors[ImGuiCol_Button] = ImVec4{0.13f, 0.13f, 0.17, 1.0f};
    colors[ImGuiCol_ButtonHovered] = ImVec4{0.19f, 0.2f, 0.25f, 1.0f};
    colors[ImGuiCol_ButtonActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
    colors[ImGuiCol_CheckMark] = ImVec4{0.74f, 0.58f, 0.98f, 1.0f};

    // Popups
    colors[ImGuiCol_PopupBg] = ImVec4{0.1f, 0.1f, 0.13f, 0.92f};

    // Slider
    colors[ImGuiCol_SliderGrab] = ImVec4{0.44f, 0.37f, 0.61f, 0.54f};
    colors[ImGuiCol_SliderGrabActive] = ImVec4{0.74f, 0.58f, 0.98f, 0.54f};

    // Frame BG
    colors[ImGuiCol_FrameBg] = ImVec4{0.13f, 0.13, 0.17, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = ImVec4{0.19f, 0.2f, 0.25f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

    // Tabs
    colors[ImGuiCol_Tab] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
    colors[ImGuiCol_TabHovered] = ImVec4{0.24, 0.24f, 0.32f, 1.0f};
    colors[ImGuiCol_TabActive] = ImVec4{0.2f, 0.22f, 0.27f, 1.0f};
    colors[ImGuiCol_TabUnfocused] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

    // Title
    colors[ImGuiCol_TitleBg] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
    colors[ImGuiCol_TitleBgActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg] = ImVec4{0.1f, 0.1f, 0.13f, 1.0f};
    colors[ImGuiCol_ScrollbarGrab] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4{0.19f, 0.2f, 0.25f, 1.0f};
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4{0.24f, 0.24f, 0.32f, 1.0f};

    // Seperator
    colors[ImGuiCol_Separator] = ImVec4{0.44f, 0.37f, 0.61f, 1.0f};
    colors[ImGuiCol_SeparatorHovered] = ImVec4{0.74f, 0.58f, 0.98f, 1.0f};
    colors[ImGuiCol_SeparatorActive] = ImVec4{0.84f, 0.58f, 1.0f, 1.0f};

    // Resize Grip
    colors[ImGuiCol_ResizeGrip] = ImVec4{0.44f, 0.37f, 0.61f, 0.29f};
    colors[ImGuiCol_ResizeGripHovered] = ImVec4{0.74f, 0.58f, 0.98f, 0.29f};
    colors[ImGuiCol_ResizeGripActive] = ImVec4{0.84f, 0.58f, 1.0f, 0.29f};

    auto &style = ImGui::GetStyle();
    style.TabRounding = 4;
    style.ScrollbarRounding = 9;
    style.GrabRounding = 3;
    style.FrameRounding = 3;
    style.PopupRounding = 4;
    style.ChildRounding = 4;
}

void SetupLoop() {
    SetStyles();
}

void TearDownGraphics() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
}

void SetupGraphics() {
    if (!glfwInit()) {
        throw std::runtime_error("Could not initialize graphics");
    }

    // Decide GL+GLSL versions
#if __APPLE__
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    // glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
#endif

    window = glfwCreateWindow(640, 480, "hook", NULL, NULL);
    if (window == NULL) {
        throw std::runtime_error("Could not create window");
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
}

void TearDownDebugger() {
    lldb::SBDebugger::Destroy(debugger);
}

void SetupDebugger() {
    lldb::SBDebugger::Initialize();
    debugger = lldb::SBDebugger::Create();
}

int main(int argc, char** argv) {
    try {
        SetupDebugger();
        SetupGraphics();

        SetupLoop();
        MainLoop();

        TearDownGraphics();
        TearDownDebugger();
    } catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
    }
}