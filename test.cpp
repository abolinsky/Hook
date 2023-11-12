#include <iostream>
#include <chrono>
#include <thread>

bool stop = false;

int main() {
    for (int i = 0; !stop;) {
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(20ms);
        std::cout << i << std::endl;
    }
    std::cout << "exited!" << std::endl;
}