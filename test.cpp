#include <iostream>
#include <chrono>
#include <thread>
#include "hook.h"

using namespace std::chrono_literals;

struct Foo {
    int a = 0;
    int b = 0;
};
std::ostream& operator<<(std::ostream& s, const Foo& foo) {
    s << "foo: (a: " << foo.a << ", b: " << foo.b << ")";
    return s;
}

class Bar {
public:
    int c = 0;

private:
    Foo foo;
    friend std::ostream& operator<<(std::ostream& os, const Bar& obj);
};
std::ostream& operator<<(std::ostream& s, const Bar& bar) {
    s << "bar: (" << bar.c << ", " << bar.foo << ")";
    return s;
}

int main() {
    Foo foo;
    Bar bar;
    bool stop = false;
    for (const int i = 0; !stop;) {
        std::this_thread::sleep_for(20ms);
        std::cout << "i: " << i << std::endl;
        std::cout << foo << std::endl;
        std::cout << bar << std::endl;
    }

    std::cout << "exited!" << std::endl;
}