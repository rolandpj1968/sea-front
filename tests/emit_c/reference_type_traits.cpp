// EXPECT: 0
// Parser must accept `__reference_constructs_from_temporary` and
// `__reference_converts_from_temporary` as type-traits taking two
// types. Used by libstdc++ ≥13's <type_traits>. We don't implement
// the semantics yet; sea-front returns false for any input. The
// test verifies the trait CALL parses and evaluates without a
// parse error / segfault.
//
// Regression for 4cbffea (parser accepts these traits via the
// `__reference_` prefix in is_type_trait).

int main() {
    // Parse + evaluate. The actual value is implementation-defined
    // (sea-front conservatively returns false until it implements
    // the semantics); we only require that the expressions compile.
    bool r1 = __reference_constructs_from_temporary(int&, int);
    bool r2 = __reference_converts_from_temporary(int&, int);
    (void)r1;
    (void)r2;
    return 0;
}
