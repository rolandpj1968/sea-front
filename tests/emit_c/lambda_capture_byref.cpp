// EXPECT: 5
// C++11 capturing lambda with explicit by-ref capture —
// N4659 §8.1.5.2 [expr.prim.lambda.capture]. Sea-front lowers
// the lambda to a synthesised free function plus a closure
// struct. By-ref captures store T* in the closure; references
// in the body emit '*__self->name' to reach the original lvalue.

int main() {
    int x = 5;
    auto f = [&x]() -> int { return x; };
    return f();
}
