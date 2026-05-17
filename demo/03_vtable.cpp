// Demo: vtables and virtual dispatch (N4659 §13.3 [class.virtual]).
//
// Each polymorphic root class gets:
//   - a vtable struct  (per-class layout of function-pointer slots)
//   - a static vtable instance pre-filled with the most-derived overrides
//   - a __sf_vptr field at offset 0 of the polymorphic root
//
// Derived classes inherit __sf_vptr through their offset-0 base; the
// derived class's vtable instance overrides the slots it re-defines
// (here, Cat::speak overrides Animal::speak).
//
// Virtual calls go via the vptr; non-virtual methods are direct C calls.
// Single-inheritance polymorphism only — multi-inheritance vtable
// thunks aren't implemented yet (the C/B/D pattern from g++.dg/ipa
// /pr46287 still needs offset-adjusting thunks).

struct Animal {
    int weight;
    Animal(int w) : weight(w) {}
    virtual int speak() { return 0; }     // "silent"
    virtual ~Animal() {}
};

struct Cat : Animal {
    Cat(int w) : Animal(w) {}             // base ctor passes through weight
    virtual int speak() { return 7; }     // "meow"
};

int describe(Animal *a) {
    return a->speak() * 10 + a->weight;   // dispatched through vptr
}

int main() {
    Animal a(3);
    Cat    c(4);
    return describe(&a) + describe(&c);   // 3 + 74 = 77
}
