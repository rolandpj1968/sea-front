#include <stdint.h>
#include <utility>

template<typename T>
struct Box {
    T val;
    T get() { return val; }
    void set(T v) { val = v; }
};

template<typename T>
T add(T a, T b) { return a + b; }

int main() {
    Box<int> b;
    b.set(20);
    
    std::pair<int, int> p;
    p.first = b.get();
    p.second = 22;
    
    return add<int>(p.first, p.second);
}
