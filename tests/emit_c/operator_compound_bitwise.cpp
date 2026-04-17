// EXPECT: 14
// Test: compound bitwise assignment operators (&=, |=, ^=) get distinct
// mangles from their non-assignment counterparts (&, |, ^).
struct Bits {
    int v;
    Bits() : v(0) {}
    Bits(int x) : v(x) {}
    Bits operator&(Bits other) { Bits r; r.v = v & other.v; return r; }
    Bits& operator&=(Bits other) { v &= other.v; return *this; }
    Bits operator|(Bits other) { Bits r; r.v = v | other.v; return r; }
    Bits& operator|=(Bits other) { v |= other.v; return *this; }
};

int main() {
    Bits a(0xFF);
    Bits b(0x0F);
    Bits c = a & b;    // 0x0F = 15
    a |= b;            // 0xFF | 0x0F = 0xFF
    a &= Bits(0x0E);   // 0xFF & 0x0E = 0x0E = 14
    return a.v;         // 14
}
