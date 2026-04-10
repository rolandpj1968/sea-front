#include <stdint.h>
#include <utility>

std::pair<int, double> make_pair_id(int a, double b) {
    std::pair<int, double> p;
    p.first = a;
    p.second = b;
    return p;
}

int main() {
    std::pair<int, double> p = make_pair_id(42, 3.14);
    return p.first;
}
