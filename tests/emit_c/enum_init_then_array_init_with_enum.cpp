// EXPECT: 30
// Verifies that pass-0a's tentative array forward decl (emitted
// BEFORE the enum bodies, to support enums whose init uses
// sizeof(arr)) does NOT break arrays whose INITIALIZER references
// enum values. The tentative decl carries no initializer, so the
// real definition (emitted later, after enums) can still use
// enum names safely.

enum K { K_A = 5, K_B = 10, K_C = 15 };

static const int arr[] = { K_A, K_B, K_C };   // init uses enum values

enum {
    SIZE = sizeof(arr) / sizeof(arr[0])       // init uses sizeof(arr)
};

int main() {
    int sum = 0;
    for (int i = 0; i < SIZE; i++) sum += arr[i];
    return sum;   // 5 + 10 + 15 = 30
}
