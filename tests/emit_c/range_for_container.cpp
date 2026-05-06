// EXPECT: 42
// C++11 range-based for over a class with begin()/end() returning
// pointers. Codegen desugars to '__sf_it = c.begin()' / '__sf_end =
// c.end()' and iterates 'for (...; __sf_it != __sf_end; ++__sf_it)'.
// N4659 §9.5.4 [stmt.ranged].

struct Vec {
    int data[4];
    int *begin() { return data; }
    int *end()   { return data + 4; }
};

int main() {
    Vec v;
    v.data[0] = 10; v.data[1] = 11; v.data[2] = 12; v.data[3] = 9;
    int sum = 0;
    for (int x : v) sum += x;
    return sum;
}
