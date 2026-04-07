// EXPECT: 12
// Multiple out-of-class methods, with parameters, called via both
// value and pointer receivers.
//   c.inc(3)  → Counter_inc(&c, 3)
//   cp->inc(5) → Counter_inc(cp, 5)   (cp is already a pointer)
//   cp->get()  → Counter_get(cp)
struct Counter {
    int count;
    void inc(int by);
    int get();
};

void Counter::inc(int by) {
    count = count + by;
}

int Counter::get() {
    return count;
}

int main() {
    Counter c;
    c.count = 0;
    c.inc(3);
    c.inc(4);
    Counter *cp = &c;
    cp->inc(5);
    return cp->get();
}
