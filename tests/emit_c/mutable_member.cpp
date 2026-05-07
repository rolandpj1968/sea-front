// EXPECT: 42
// N4659 §10.1.1/8 [dcl.stc]: 'mutable' on a class data member lets a
// const member function modify it. C has no mutable, so sea-front
// must cast away constness at the write site. Real-world hit: gcc 14
// libcpp/line-map.cc rich_location's mutable m_have_expanded_location
// + m_expanded_location, written from const member functions.
struct Cache {
    mutable int last;
    Cache() : last(0) {}
    int store(int x) const {
        last = x;          // bare-name write through 'this' (const method)
        return last;
    }
};

struct Outer {
    mutable Cache c;
    int probe(int x) const {
        c.last = x;        // chain: outer 'c' is mutable, leaf 'last' regular
        return c.last;
    }
};

int main() {
    Outer o;
    return o.probe(42);
}
