// EXPECT: 1
// C requires struct bodies to declare at least one named member
// (C11 §6.7.2.1/1). An empty C++ class — bodies-only, no data
// members, no bases, no vtable — lowers to a struct that gcc/clang
// accept as an extension but cproc rejects. Sea-front emits a
// 'char __sf_empty;' placeholder field so the lowering is portable.
//
// sizeof becomes 1, matching C++'s own empty-class rule
// (§6.7.2/2: complete object types have non-zero size).

struct Empty {
    void greet() const {}
};

int main() {
    Empty e;
    e.greet();
    return sizeof(Empty);
}
