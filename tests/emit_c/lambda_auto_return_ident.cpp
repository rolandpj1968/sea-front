// EXPECT: 42
// Auto-return deduction from an ident — lookup_unqualified gives
// the parser the ident's Type at lambda parse time (the body has
// already parsed and the body's REGION_BLOCK has popped). The
// captured 'x' is then used as the deduced return type.

int main() {
    int x = 7;
    auto f = [&]() { return x; };  // deduce → int via ident lookup
    return f() * 6;
}
