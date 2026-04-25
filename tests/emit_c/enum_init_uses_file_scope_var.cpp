// EXPECT: 1
// File-scope enum whose initializer references a file-scope array
// variable defined LEXICALLY EARLIER. C99 allows this — the array's
// size is a compile-time constant after the array is declared, and
// sizeof on it works in any constant context. Sea-front must emit
// the variable before the enum.
//
// Pattern: gcc 4.8 c-family/c-pch.c —
//   static const struct ... pch_matching[] = { ... };
//   enum { MATCH_SIZE = ARRAY_SIZE (pch_matching) };

static const int arr[] = { 7, 8 };

enum {
    SIZE_OF_ARR = sizeof(arr) / sizeof(arr[0])
};

int main() {
    return SIZE_OF_ARR == 2 ? 1 : 0;   // 1 if order preserved
}
