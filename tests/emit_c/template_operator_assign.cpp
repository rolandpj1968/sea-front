// EXPECT: 25
// Test: operator+= rewrite works inside template-instantiated methods.
// Exercises: resolved_type propagation through clone + subst_type,
// ND_ASSIGN rewrite on struct with class_type in template context.
template<typename T>
struct Acc {
    T val;
    Acc() : val(0) {}
    Acc(T x) : val(x) {}
    Acc(const Acc& o) : val(o.val) {}
    Acc& operator+=(T x) { val += x; return *this; }
    Acc plus(T x) {
        Acc copy(*this);
        copy += x;
        return copy;
    }
};

int main() {
    Acc<int> a(20);
    Acc<int> b = a.plus(5);
    return b.val; // 25
}
