// EXPECT: 33
// Distinction rule: enum tag — N4659 §10.2 [dcl.enum]. f(enum A) and
// f(enum B) are distinct overloads. Pattern: gcc 4.8 gimple.h's
// gimple_call_builtin_p(enum built_in_class) vs (enum built_in_function).

enum Color    { RED = 1,  GREEN = 2 };
enum Material { GLASS = 10, IRON = 20 };

int classify(Color c)    { return c; }
int classify(Material m) { return m; }

int main() {
    return classify(GREEN) + classify(IRON) + classify(RED) + classify(GLASS);
    // 2 + 20 + 1 + 10 = 33
}
