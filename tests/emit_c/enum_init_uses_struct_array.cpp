// EXPECT: 1
// The c-pch.c shape: file-scope array of struct, then enum whose
// initializer takes sizeof(arr). Pass-0b hoists the var decl (with
// inline struct body + init) before the enum bodies so sizeof()
// sees a complete type.
//
// Pattern: gcc 4.8 c-family/c-pch.c
//   static const struct c_pch_matching { ... } pch_matching[] = { ... };
//   enum { MATCH_SIZE = ARRAY_SIZE (pch_matching) };

struct E { int a; int b; };

static const struct E pch_arr[] = { { 1, 2 }, { 3, 4 } };

enum {
    PCH_SIZE = sizeof(pch_arr) / sizeof(pch_arr[0])
};

int main() {
    return PCH_SIZE == 2 ? 1 : 0;
}
