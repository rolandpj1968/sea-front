// EXPECT: 7
// Pointer-passing function: tests that *p = ... assignment goes through
// pointer indirection. swap_to(&x, 7) makes x = 7, then return x.
void swap_to(int *p, int v) {
    *p = v;
}

int main() {
    int x = 0;
    swap_to(&x, 7);
    return x;
}
