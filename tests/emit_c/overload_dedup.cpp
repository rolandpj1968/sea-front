// EXPECT: 3
// Test: function declaration dedup — same function declared multiple
// times (e.g. through headers) doesn't produce redefinition errors
int foo(int x);
int foo(int x);
int foo(int x) { return x; }

int main() {
    return foo(3);
}
