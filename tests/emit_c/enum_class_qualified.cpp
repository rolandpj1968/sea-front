// EXPECT: 7
// Scoped-enum member access via qualified-id — N4659 §10.2/5
// [dcl.enum]: scoped enumerators ('enum class' / 'enum struct') are
// only reachable through 'EnumName::value'. Real-world hit: gcc 14
// libcpp/lex.cc uses 'case kind::NONE:' inside a switch.
//
// Sea-front emits the enum body as a flat C 'enum K { NONE, ... }',
// so the enumerator lives at TU scope as a regular C constant; the
// codegen ND_QUALIFIED handler bare-emits the trailing identifier
// when parts[0] resolves to an enum tag.

enum class kind { NONE = 0, ONE = 1, SEVEN = 7 };

int classify(kind k) {
    switch (k) {
        case kind::SEVEN: return 7;
        case kind::ONE:   return 1;
        case kind::NONE:  return 0;
    }
    return -1;
}

int main() {
    return classify(kind::SEVEN);
}
