// EXPECT: 33
// Two free-function overloads differing only by enum parameter
// type. ffsig_fill must distinguish them by enum tag — otherwise
// overload-mangling is skipped and the two emit with the same C
// name, producing 'conflicting types' at the C compiler.
//
// Pattern: gcc 4.8 gimple.h gimple_call_builtin_p — one overload
// takes `enum built_in_class`, the other `enum built_in_function`.

enum Color    { RED = 1,  GREEN = 2 };
enum Material { GLASS = 10, IRON = 20 };

int classify(Color c)    { return c; }
int classify(Material m) { return m; }

int main() {
    return classify(GREEN) + classify(IRON) + classify(RED) + classify(GLASS);
    // 2 + 20 + 1 + 10 = 33
}
