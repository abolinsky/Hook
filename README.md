<p align="center">
  <img width="256" height="256" src="https://github.com/abolinsky/hook/assets/5623716/6dcca51d-8cd3-4e87-b2f9-59be47c76def"/>
</p>

# hook
A **graphical C/C++ runtime editor** for rapid experimentation. It attaches to your running program and allows you to easily change the value of variables live, breaking the time-consuming edit-compile-run-edit cycle.

<img width="1141" alt="hook_in_action" src="https://github.com/abolinsky/hook/assets/5623716/0f699866-4934-4e79-991b-07e6579bed36">

# dependencies
## glfw
### macOS
```
brew install glfw
```

## lldb
https://lldb.llvm.org/resources/build.html

# build
```
cmake -B build -S .
cmake --build build
sudo cmake --install build
```