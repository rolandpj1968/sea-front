// EXPECT: 42
// Template body containing GNU statement-expression — clone.c must
// recurse into ND_STMT_EXPR's block when cloning the template body.
// Without this case, the cloned stmt-expr emits as empty '()' and
// the surrounding cast collapses into a no-op assignment.
//
// Real-world hit: gcc 14 libcpp/identifiers.cc
//   template<typename Node>
//   static hashnode alloc_node (cpp_hash_table *table) {
//       const auto node = XOBNEW (&table->pfile->hash_ob, Node);
//       ...
//   }
// where XOBNEW expands to a nested __extension__ ({...}) chain.
//
// Sea-front captures statement-expressions as a structured AST
// (ND_STMT_EXPR with a block child) rather than as a raw token
// range, so cloning under template instantiation must walk into
// the block. The bug was a missing case in clone_node — the
// default fell through with kind/tok/resolved_type only, dropping
// stmt_expr.block.

template <typename T>
T compute(T base) {
    // Stmt-expr inside a template body — block references the
    // template parameter T (via the typed locals + initialiser).
    T result = ({
        T tmp = base;
        tmp = tmp + 32;
        tmp;
    });
    return result;
}

int main() {
    return compute<int>(10);
}
