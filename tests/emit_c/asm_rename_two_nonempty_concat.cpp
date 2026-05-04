// EXPECT: 99
// __asm("a" "b") concatenates per N4659 §5.13.5/13 [lex.string].
// Sibling test to asm_rename_concatenated_string.cpp — that one covers
// the leading-empty 'asm("" "name")' shape; this one covers TWO
// genuinely non-empty literals 'asm("a" "b")', which the
// "last-non-empty wins" stopgap would have miscompiled.

extern "C" int real_value();
extern "C" int fancy() __asm__("real_v" "alue");

int real_value() { return 99; }

int main() {
    return fancy();
}
