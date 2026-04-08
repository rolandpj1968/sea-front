// EXPECT: 12
// Member dtor + user dtor both fire. C++ runs the user body
// FIRST, then implicitly destroys members in reverse declaration
// order (N4659 §15.4 [class.dtor]/9).
//
// ~Outer body sets g = 1. Then ~Inner runs and does g = g*10 + 2.
// Final g = 12.
int g = 0;

struct Inner {
    ~Inner() { g = g * 10 + 2; }
};

struct Outer {
    Inner i;
    ~Outer() { g = 1; }
};

int main() {
    {
        Outer o;
    }
    return g;
}
