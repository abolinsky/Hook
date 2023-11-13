#include <iostream>
#include <chrono>
#include <thread>

#ifdef HOOK
#define const volatile
#endif

using namespace std::chrono_literals;

int main() {
    bool stop = false;
    for (const int i = 0; !stop;) {
        std::this_thread::sleep_for(20ms);
        std::cout << i << std::endl;
    }

    std::cout << "exited!" << std::endl;
}