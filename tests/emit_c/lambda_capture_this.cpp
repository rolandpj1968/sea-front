// EXPECT: 22
// '[this]' capture — N4659 §8.1.5.2/8 [expr.prim.lambda.capture].
// Closure stores 'Class *' at member '__this'; bare 'this' in the
// lambda body emits as '__self->__this', and implicit-this member
// references emit as '__self->__this->member' (instead of the
// usual 'this->member' which would be wrong inside the lambda fn,
// where 'this' isn't a parameter).
struct Foo {
    int v;
    int run(int x) {
        auto f = [this](int a) -> int { return a + v; };          // implicit-this
        auto g = [this]() -> int { return this->v * 2; };          // explicit this->
        return f(x) + g();
    }
};
int main() {
    Foo foo;
    foo.v = 5;
    // f(7) = 7+5 = 12; g() = 5*2 = 10; total = 22
    return foo.run(7);
}
