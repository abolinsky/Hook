#include "backend.h"

#include <mach-o/dyld.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <lldb/API/LLDB.h>

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <limits>


namespace Hook {

std::string GetExecutablePath() {
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) {
        throw std::runtime_error("Could not get executable path");
    }

    return std::string(path);
}

std::string GetDebugServerPath() {
    std::string executablePath = GetExecutablePath();
    size_t appDirPos = executablePath.find(".app");
    if (appDirPos == std::string::npos) {
        throw std::runtime_error("Could not get debugserver path");
    }

    std::string debugServerPath = executablePath.substr(0, appDirPos) + ".app/Contents/Frameworks/bin/debugserver";
    return debugServerPath;
}

struct VariableInfo {
    VariableInfo(lldb::SBValue& value) {
        this->name = value.GetName();
        this->function_name = (name.substr(0,2) == "::") ? "" : value.GetFrame().GetFunctionName();
        this->id = value.GetID();

        this->type = value.GetType().GetCanonicalType();
        while (type.GetTypeClass() == lldb::eTypeClassTypedef) {
            this->type.GetTypedefedType();
        }
        this->type_name = this->type.GetName();
        this->type_class = this->type.GetTypeClass();
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

    std::string GetFullyQualifiedValue() const {
        if (type_class == lldb::eTypeClassEnumeration) {
            return type_name + "::" + value;
        } else {
            return value;
        }
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
    std::string type_name;
    lldb::TypeClass type_class;
    lldb::BasicType basic_type;
    bool is_nested = false;
    uint64_t id = std::numeric_limits<uint64_t>::max();
    std::vector<VariableInfo*> children;
    VariableInfo* parent = nullptr;
};

std::vector<VariableInfo> variables;
const VariableInfo* current_var_info;
bool published_changes = true;
bool open_pid_popup = true;

lldb::SBDebugger debugger;
lldb::SBTarget target;
lldb::SBListener listener;
lldb::SBError error;
lldb::SBProcess process;

lldb::pid_t pid = 0;

void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

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

void DisplayVariable(VariableInfo& varInfo) {
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
        if (varInfo.type_class == lldb::eTypeClassEnumeration) {
            auto members = varInfo.type.GetEnumMembers();
            auto num_members = members.GetSize();
            std::vector<std::string> member_names;
            for (unsigned i = 0; i < num_members; ++i) {
                auto current_enum_member = members.GetTypeEnumMemberAtIndex(i);
                if (current_enum_member.IsValid() && std::string(current_enum_member.GetName()) != "") {
                    member_names.push_back(current_enum_member.GetName());
                }
            }
            int index = 0;
            for (auto& n : member_names) {
                if (varInfo.value == n) {
                    std::string inputTextLabel = "##sliderEnum" + varInfo.name;
                    ImGui::SliderInt(inputTextLabel.c_str(), &index, 0, member_names.size() - 1, member_names[index].c_str());
                    varInfo.value = member_names[index];
                    break;
                }
                ++index;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                PublishChange(varInfo);
            }
        } else if (varInfo.basic_type == lldb::eBasicTypeBool) {
            bool value = varInfo.value == "true";
            ImGui::Checkbox(inputTextLabel.c_str(), &value);
            varInfo.value = value ? "true" : "false";
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                PublishChange(varInfo);
            }
        } else if (varInfo.basic_type == lldb::eBasicTypeUnsignedChar) {
            uint8_t value = std::stoul(varInfo.value);
            static auto min = std::numeric_limits<uint8_t>::min();
            static auto max = std::numeric_limits<uint8_t>::max();
            ImGui::SliderScalar(inputTextLabel.c_str(), ImGuiDataType_U8, &value, &min , &max);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                PublishChange(varInfo);
            }
            ImGui::SameLine(); HelpMarker("CTRL+click to input value");
            varInfo.value = std::to_string(value);
        } else if (varInfo.basic_type == lldb::eBasicTypeInt) {
            int value = std::stoi(varInfo.value);
            static auto min = std::numeric_limits<int>::min() / 2;
            static auto max = std::numeric_limits<int>::max() / 2;
            ImGui::SliderScalar(inputTextLabel.c_str(), ImGuiDataType_S32, &value, &min , &max);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                PublishChange(varInfo);
            }
            ImGui::SameLine(); HelpMarker("CTRL+click to input value");
            varInfo.value = std::to_string(value);
        } else {
            ImGui::InputText(inputTextLabel.c_str(), &varInfo.value, ImGuiInputTextFlags_CharsDecimal);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                PublishChange(varInfo);
            }
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

void StyleColorsFunky() {
    auto yellow = ImVec4{0.996, 0.780, 0.008, 1.0};
    auto blue = ImVec4{0.090, 0.729, 0.808, 1.0};
    auto green = ImVec4{0.149, 0.918, 0.694, 1.0};
    auto white = ImVec4{0.996, 0.996, 0.996, 1.0};
    auto middle_gray = ImVec4{0.5f, 0.5f, 0.5f, 1.0};
    auto light_gray = ImVec4{0.85f, 0.85f, 0.85f, 1.0};
    auto off_white = ImVec4{0.96f, 0.96f, 0.96f, 1.0};
    auto black = ImVec4{0.0, 0.0, 0.0, 1.0};

    auto &colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg] = green;
    colors[ImGuiCol_MenuBarBg] = blue;

    // Border
    colors[ImGuiCol_Border] = white;
    colors[ImGuiCol_BorderShadow] = ImVec4{0.0f, 0.0f, 0.0f, 0.24f};

    // Text
    colors[ImGuiCol_Text] = black;
    colors[ImGuiCol_TextDisabled] = middle_gray;

    // Headers
    colors[ImGuiCol_Header] = yellow;
    colors[ImGuiCol_HeaderHovered] = light_gray;
    colors[ImGuiCol_HeaderActive] = white;

    // Buttons
    colors[ImGuiCol_Button] = blue;
    colors[ImGuiCol_ButtonHovered] = white;
    colors[ImGuiCol_ButtonActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
    colors[ImGuiCol_CheckMark] = blue;

    // Popups
    colors[ImGuiCol_PopupBg] = white;

    // Slider
    colors[ImGuiCol_SliderGrab] = yellow;
    colors[ImGuiCol_SliderGrabActive] = white;

    // Frame BG
    colors[ImGuiCol_FrameBg] = off_white;
    colors[ImGuiCol_FrameBgHovered] = light_gray;
    colors[ImGuiCol_FrameBgActive] = yellow;

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
    style.WindowBorderSize = 0;
    style.GrabRounding = 3;
    style.FrameRounding = 3;
    style.PopupRounding = 4;
    style.ChildRounding = 4;
}

void StyleColorsBlack() {
    auto &colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg] = ImVec4{0.1f, 0.1f, 0.13f, 1.0};
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
    style.WindowBorderSize = 0;
    style.GrabRounding = 3;
    style.FrameRounding = 3;
    style.PopupRounding = 4;
    style.ChildRounding = 4;
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
    
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Attach with PID", "Ctrl+A")) {
                open_pid_popup = true;
            }
            if (ImGui::BeginMenu("Theme")) {
                if (ImGui::MenuItem("Dark", "Ctrl+D")) {
                    ImGui::StyleColorsDark();
                }
                if (ImGui::MenuItem("Light", "Ctrl+L")) {
                    ImGui::StyleColorsLight();
                }
                if (ImGui::MenuItem("Classic", "Ctrl+C")) {
                    ImGui::StyleColorsClassic();
                }
                if (ImGui::MenuItem("Black", "Ctrl+B")) {
                    StyleColorsBlack();
                }
                if (ImGui::MenuItem("Funky", "Ctrl+F")) {
                    StyleColorsFunky();
                }
                ImGui::EndMenu();
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
        if (ImGui::InputText("##pid", &pidInput, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
            pid = std::stoull(pidInput);
            try {
                HandleAttachProcess();
                ImGui::CloseCurrentPopup();
                attach_failed = false;
            } catch (const std::exception& e) {
                std::cerr << e.what() << std::endl;
                attach_failed = true;
            }
        }
        // TODO: progress bar

        if (attach_failed) {
            ImGui::BeginDisabled();
            ImGui::TextColored(ImVec4{1.000, 0.353, 0.322, 1.0}, "Error: Could not attach to pid %llu", pid);
            ImGui::EndDisabled();
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
    std::string expression = fully_qualified_name + " = " + var_info->GetFullyQualifiedValue();

    for (auto& frame : GetFrames(thread)) {
        lldb::SBValue var = FindVariableById(frame, var_info->GetRoot().id);
        if (!var) continue;

        lldb::SBValue value = frame.EvaluateExpression(expression.c_str());
        if (value && value.GetValue()) return;
    }

    std::cerr << "Failed to evaluate " << expression << std::endl;
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

void HandleKeys() {
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            open_pid_popup = true;
        } else if (ImGui::IsKeyDown(ImGuiKey_D)) {
            ImGui::StyleColorsDark(nullptr);
        } else if (ImGui::IsKeyDown(ImGuiKey_C)) {
            ImGui::StyleColorsClassic(nullptr);
        } else if (ImGui::IsKeyDown(ImGuiKey_L)) {
            ImGui::StyleColorsLight(nullptr);
        } else if (ImGui::IsKeyDown(ImGuiKey_B)) {
            StyleColorsBlack();
        } else if (ImGui::IsKeyDown(ImGuiKey_F)) {
            StyleColorsFunky();
        }
    } else if (ImGui::IsKeyDown(ImGuiKey_S)) {
        process.Stop();
    }
}

void core() {
    HandleKeys();
    HandleLLDBProcessEvents();
    Draw();
}

void SetupLoop() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    StyleColorsBlack();
}

void TearDownDebugger() {
    lldb::SBDebugger::Destroy(debugger);
}

void SetupDebugger() {
    setenv("LLDB_DEBUGSERVER_PATH", GetDebugServerPath().c_str(), 1);
    lldb::SBDebugger::Initialize();
    debugger = lldb::SBDebugger::Create();
}

}

int main() {
    using namespace Hook;
    try {
        SetupDebugger();
        SetupLoop();
        main_loop(core);
        TearDownDebugger();
    } catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
    }
}
