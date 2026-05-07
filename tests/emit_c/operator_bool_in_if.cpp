// EXPECT: 7
// N4659 §7.3 [conv]/3 + §16.3.1.6 [over.match.copy]: contextual
// conversion to bool in an if condition invokes a class's
// `operator bool()` (or other matching conversion function). C has
// no such mechanism, so emit rewrites the condition to a method call.
// Real-world hit: gcc 14 libcpp/lex.cc scan_id_result with
// `explicit operator bool() const { return node; }` used as
// `if (const auto sr = scan_cur_identifier(pfile))`.
struct Box {
    int v;
    explicit operator bool() const { return v != 0; }
};

int main() {
    Box b;
    b.v = 42;
    Box c;
    c.v = 0;

    int sum = 0;
    if (b) sum += 1;          // direct: struct in bool context
    if (c) sum += 100;        // false → no add
    if (const Box d = b) sum += 2; // init-decl: struct in bool context
    if (const Box e = c) sum += 200;  // false → no add
    sum += 4;                 // unconditional
    return sum;               // 1 + 2 + 4 = 7
}
