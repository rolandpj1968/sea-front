// EXPECT: 0
// Out-of-line definition of a conversion operator `T::operator U()
// const { ... }` — N4659 §11.4.8 [class.conv.fct]. The OOL form
// requires the parser to recognise `Class::operator` as a qualified
// conversion-operator name in decl-spec position (afa6b51).
//
// We only exercise the boolean conversion here because it's the one
// sea-front actually invokes implicitly (via if/while bool context);
// other conversion operators parse but don't auto-fire on implicit
// conversion (deferred slice).

int conv_bool_calls = 0;

struct C {
    int v;
    C(int x) : v(x) {}
    operator bool() const;
};

C::operator bool() const {
    ++conv_bool_calls;
    return v != 0;
}

int main() {
    C c(7);
    int saw_true  = 0;
    int saw_false = 0;
    if (c) saw_true = 1;
    C zero(0);
    if (zero) {} else saw_false = 1;
    if (saw_true != 1)  return 1;
    if (saw_false != 1) return 2;
    if (conv_bool_calls != 2) return 3;
    return 0;
}
