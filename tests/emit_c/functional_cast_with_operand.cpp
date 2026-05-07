// EXPECT: 6
// N4659 §8.2.3 [expr.type.conv]: a simple-type-specifier followed by
// '(expression)' is a unary explicit type conversion (functional cast).
// The operand must reach the AST so emit can render it. The previous
// skip-balanced + NULL-operand path emitted as ((T*)0), silently
// miscompiling expressions like `int(token->type == X)` in
// gcc 14 libcpp/charset.cc — `2 * int(predicate)` became `2 * (int*)0`.
int main() {
    bool b = true;
    int x = int(b) + int(b == true) * 4 + int(false);  // 1 + 4 + 0 = 5
    return x + 1; // 6
}
