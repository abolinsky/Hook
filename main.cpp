#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

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
    GLFWwindow* window = glfwCreateWindow(640, 480, "Dear ImGui GLFW+OpenGL3 example", NULL, NULL);
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

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create a window with some text fields
        ImGui::Begin("Example Window");
        static char text[128] = "";
        ImGui::InputText("Text field", text, IM_ARRAYSIZE(text));
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
