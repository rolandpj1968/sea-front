#include <stdint.h>
#include <vector>

int main() {
    std::vector<int> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(12);
    return v[0] + v[1] + v[2];
}
