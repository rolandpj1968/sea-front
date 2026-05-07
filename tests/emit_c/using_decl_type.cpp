// EXPECT: 7
// using-declaration: 'using ns::name' brings 'name' into the
// enclosing scope (N4659 §10.3.3 [namespace.udecl]/4). Sea-front
// previously skipped the using-decl without recording 'name', so
// downstream uses of the introduced name in type position misparsed
// as expressions.
//
// Concretely we test that 'rethrow_exception(exception_ptr)' style
// declarations parse — the qualifier brings the type-name into the
// enclosing namespace and a parameter named after a forward-declared
// class no longer trips the most-vexing-parse heuristic.
namespace inner { class Tag; }
using inner::Tag;
namespace outer {
    void use(Tag *);   // 'Tag' as a parameter type — parser must
                       // recognise it as a type-name to avoid the
                       // most-vexing-parse fallthrough.
}
int main() { return 7; }
