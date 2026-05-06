// EXPECT: 42
// C++11 range-based for over a built-in array — N4659 §9.5.4
// [stmt.ranged]. Codegen desugars to 'T* it = arr, *end = arr+N'
// and iterates with pointer comparison.

int main() {
    int arr[4] = {10, 11, 12, 9};
    int sum = 0;
    for (int x : arr) sum += x;
    return sum;
}
