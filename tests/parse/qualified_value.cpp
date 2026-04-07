// parser_at_type_specifier now narrows qualified names via real
// lookup. 'std::cout' is a VARIABLE, not a type — so a statement
// 'std::cout << 42;' is parsed as an expression-statement, not as
// a (failed) declaration.

namespace ns {
    int answer = 42;
    typedef int integer;
    struct Box { int v; };
}

int main() {
    // ns::answer is a value — must NOT be treated as a type-specifier.
    // The whole statement should parse as an expression-statement.
    int x = ns::answer + 1;

    // ns::integer IS a type — should still be recognised as a
    // type-specifier and parse as a declaration.
    ns::integer y = 7;

    // ns::Box is a class — declaration with direct-init.
    ns::Box b;
    b.v = 3;

    return x + y + b.v;
}
