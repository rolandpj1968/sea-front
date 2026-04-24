// EXPECT: 42
// ND_COMMA and ND_BINARY have DIFFERENT union-member layouts in Node:
// ND_BINARY's struct leads with a TokenKind 'op' field, while ND_COMMA's
// does not. Sema's dispatch table previously aliased them, calling
// visit_binary on an ND_COMMA and reading n->binary.lhs / n->binary.rhs.
// Due to the struct layout offset, n->binary.lhs on an ND_COMMA node
// actually reads comma.rhs, and n->binary.rhs reads past the end.
// Result: the LHS of every comma expression was never visited, so its
// subscript/member/call nodes never got resolved_type set, and codegen
// couldn't dispatch through operator[] / method calls inside a comma.
//
// Pattern: gcc 4.8 tree.h FOR_EACH_CONSTRUCTOR_VALUE
//   for (i = 0; ...?false:((val = (*V)[i].value), true); i++)
// The '(val = (*V)[i].value)' inside the comma needs subscript dispatch.

struct Vec {
    int data[4];
    int operator[](unsigned i) const { return data[i]; }
};

int main() {
    Vec v;
    v.data[0] = 42;
    int x = 0, y = 0;
    unsigned i = 0;
    y = (i >= 4) ? 0 : ((x = v[i]), 1);   // comma lhs contains operator[] dispatch
    (void)y;
    return x;
}
