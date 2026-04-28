// EXPECT: 5
// gcc 4.8 hash_table::dispose pattern: a local variable inside a
// class-template method body shadows a class member of the same
// name. Sema must use the local, not insert this->.
//
// In the cloned template body, sea-front previously cleared
// block.scope (per clone.c) and visit_block then skipped scope
// creation, so local var declarations were never registered in
// any region. Lookup of the bare name walked out to the class
// scope and found the member — sema marked implicit_this and
// codegen emitted 'this->n' instead of the local 'n'.
//
// Reduced from hash-table.h:529 'size_t size = htab->size;
// for (int i = size - 1; ...)' where on the second reference
// 'size' (a LOCAL) became 'this->size' even though hash_table
// has no member 'size' at all — produced 'has no member named
// size' compile errors across asan.c, dse.c, valtrack.c, and
// ~25 other gcc 4.8 .c files.
//
// Standard: N4659 §6.3.3 [basic.scope.block] — a name declared
// in a block has scope from declarator to closing brace; +
// §6.3.10 [basic.scope.hiding] — block-scope names hide
// enclosing class members of the same name.

template<typename T>
struct Box {
    T *p;
    int n;
    int compute() {
        int n = 5;        // local — shadows nothing relevant
        return n + 0;     // must use the local, NOT this->n
    }
};

int main() {
    Box<int> b; b.n = 99;
    return b.compute();
}
