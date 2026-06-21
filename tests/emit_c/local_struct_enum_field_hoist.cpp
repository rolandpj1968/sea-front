// EXPECT: 5
// Function-local struct uses a function-local named enum as a field
// type. Sea-front hoists the struct (it backs a TU-scope-equivalent
// static-storage object) but the enum body originally stayed at
// function scope — making the struct field's `enum X` type
// incomplete at the file-scope struct definition, which C rejects
// (§6.7.2.3 disallows enum forward declarations).
//
// Fix: when emitting a struct, hoist the body of any named enum
// referenced by a field but not yet emitted. find_enum_def_type_by_tag
// walks the TU (including function bodies) to locate the enum body.
//
// Real-world hit: gcc 4.8 config/i386/i386.c
//   ix86_valid_target_attribute_inner_p() declares
//     enum ix86_opt_type { ix86_opt_unknown, ix86_opt_yes, ... };
//     static const struct { ...; enum ix86_opt_type type; ... } attrs[];

extern "C" void abort();

int run() {
    enum kind { K_A, K_B, K_C };
    static const struct {
        const char *name;
        enum kind k;
    } entries[] = {
        { "alpha", K_A },
        { "beta",  K_B },
        { "gamma", K_C },
    };
    int sum = 0;
    for (int i = 0; i < 3; i++) sum += entries[i].k + 1;
    return sum;
}

int main() {
    if (run() != 1 + 2 + 3) abort();
    return 5;
}
