// EXPECT: 4
// Top-level variable referenced via sizeof in a struct member
// array dimension. emit_c emits a tentative declaration of the
// variable in pass 0 so the struct member array dim resolves
// in PHASE_STRUCTS. C99 §6.9.2 allows multiple type-consistent
// tentative defs of file-scope variables. Pattern from gcc 4.8
// ggc-page.c (extra_order_size_table).

static const int sizes[4] = { 1, 2, 3, 4 };

struct Box {
    int slots[sizeof(sizes) / sizeof(sizes[0])];  // 4
};

int main() {
    struct Box b;
    int n = sizeof(b.slots) / sizeof(b.slots[0]);
    return n;
}
