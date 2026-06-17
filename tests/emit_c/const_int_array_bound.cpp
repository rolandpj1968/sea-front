// EXPECT: 0
// N4659 §8.6 [expr.const] / §11.3.4 [dcl.array] — a const int with
// a constant initialiser is a constant expression usable as an
// array bound. C requires array bounds at file scope to be
// integer-constant-expressions (literal-foldable); a use like
//   `T arr[num_vars * depth];`
// where `num_vars`/`depth` are `const int = N;` declarations would
// otherwise emit a C99 VLA shape — illegal at file scope.
//
// Sea-front now folds `const int x = literal;` decls at parse-time
// (Declaration.const_int_value) and resolves array-bound
// expressions to literal sizes via fold_const_int.
//
// Reduced from g++.dg/eh/registers1.C.

extern "C" void abort();

const int num_vars = 16;
const int depth    = 3;

float float_src[num_vars * depth];
int   int_src[num_vars * depth];

int main() {
    if (sizeof(float_src) / sizeof(float_src[0]) != 48) abort();
    if (sizeof(int_src)   / sizeof(int_src[0])   != 48) abort();
    return 0;
}
