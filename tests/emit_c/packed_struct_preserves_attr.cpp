// EXPECT: 0
// __attribute__((packed)) on a struct must reach the emitted C
// declaration so cc lays out fields with no padding. Sea-front
// captures it on Type.is_packed at parse time (parse_type_specifiers
// handles both leading `struct __attribute__((packed)) S {...}` and
// trailing `} __attribute__((packed))` positions); emit_struct_decl
// stamps the attribute on the C struct definition.
//
// Regression for e346c98. Companion to packed_enum_preserves_attr.cpp.
//
// Field sizes: char(1) + int(4) + char(1) = 6 packed, vs. natural
// layout with alignment padding = 12 (char + 3 pad + int + char + 3
// pad). Sizeof distinguishes the two unambiguously.

struct __attribute__((packed)) Packed {
    char c0;
    int i;
    char c1;
};

struct Natural {
    char c0;
    int i;
    char c1;
};

int main() {
    if (sizeof(Packed) != 6) return 1;
    if (sizeof(Natural) <= sizeof(Packed)) return 2;
    return 0;
}
