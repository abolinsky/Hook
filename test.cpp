#include <iostream>

int main() {
    bool stop = false;
    std::cout << &stop << " " << typeid(stop).name() << std::endl;
    while (!stop) {
    }
    std::cout << "exited!" << std::endl;
}
