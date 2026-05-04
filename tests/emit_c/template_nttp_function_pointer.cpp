// EXPECT: 7
// Non-type template parameter (function pointer) bound from an
// explicit template-id at a member-access call. N4659 §17.1/4
// [temp.param] permits NTTPs of pointer/function-pointer type;
// §17.7.1 [temp.inst] says each distinct (template, args) tuple
// produces a distinct entity. The instantiator must propagate the
// NTTP binding into the cloned body so a bare-name reference to
// the parameter (`F(x)`) lowers to a call of the bound function.

struct Holder {
    int field;
    template<typename T, int (*F)(T)>
    int run(T x) { return F(x) + this->field; }
};

int twice(int n) { return n + n; }

int main() {
    Holder h;
    h.field = 1;
    return h.run<int, twice>(3);
}
