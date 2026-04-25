// EXPECT: 42
// Multi-declarator for-init: parser produces ND_BLOCK; emit_c
// must render comma-separated declarators with shared base type.
// Pattern from gcc 4.8 valtrack.c.

int main() {
    int total = 0;
    int data[3] = { 10, 20, 12 };
    for (int *a = data, **b = &a; a < data + 3; a++) {
        total += *(*b);
    }
    return total;
}
