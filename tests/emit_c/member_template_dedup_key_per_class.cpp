// EXPECT: 30
// gcc 4.8 is_a_helper<T>::cast<U>(p) pattern with TWO distinct
// class instantiations: is_a_helper<Cgraph> and is_a_helper<Varpool>.
// The member-template deduced map carries U=Base (deduced from p),
// but T=Cgraph vs T=Varpool comes from the CLASS template's lead_tid,
// NOT from member-template deduction. Without including the class
// args in the dedup key, the second class instantiation collides
// with the first — only one cast def is emitted, the other call
// links against a missing symbol.
//
// Standard: N4659 §17.7.1 [temp.inst] (each distinct argument set
// across ALL heads produces a distinct specialization). The dedup
// key must reflect both heads' argument sets.

struct Base { int code; };
struct Cgraph : Base { int x; };
struct Varpool : Base { int y; };

template<typename T>
struct is_a_helper {
    template<typename U>
    static T *cast(U *p) { return (T*)p; }
};

int main() {
    Cgraph c;  c.code = 0; c.x = 10;
    Varpool v; v.code = 1; v.y = 20;
    Base *bc = &c;
    Base *bv = &v;
    Cgraph *cp  = is_a_helper<Cgraph>::cast(bc);
    Varpool *vp = is_a_helper<Varpool>::cast(bv);
    return cp->x + vp->y;   // 10 + 20 = 30
}
