CXX="clang++"
EXE="hook"
IMGUI_DIR="dependencies/imgui"

SOURCES="main.cpp "
SOURCES+="$IMGUI_DIR/imgui.cpp $IMGUI_DIR/imgui_stdlib.cpp $IMGUI_DIR/imgui_draw.cpp $IMGUI_DIR/imgui_tables.cpp $IMGUI_DIR/imgui_widgets.cpp "
SOURCES+="$IMGUI_DIR/backends/imgui_impl_glfw.cpp $IMGUI_DIR/backends/imgui_impl_metal.mm "

INCLUDES="-I$IMGUI_DIR -I$IMGUI_DIR/backends "

LIBS="-framework Metal -framework MetalKit -framework Cocoa -framework IOKit -framework CoreVideo -framework QuartzCore "
LIBS+="-I/usr/local/include/lldb -L/usr/local/lib/lldb -llldb "
LIBS+="-I/opt/homebrew/include -L/opt/homebrew/lib -lglfw "

CXXFLAGS="-std=c++20 -ObjC++ "
CXXFLAGS+="-Wall -Wformat -Wl,-rpath,/usr/local/lib/lldb"

$CXX $CXXFLAGS $LIBS $SOURCES $INCLUDES -o $EXE