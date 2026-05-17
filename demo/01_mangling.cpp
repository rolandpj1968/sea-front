// Demo: name mangling — namespaces, methods, member overloads, operators.
//
// Sea-front's human-readable scheme makes the generated names speakable:
//   geom::Vec::dot(Vec const&) const  ->  sf__geom__Vec__dot_p_geom__Vec_rc_pe__const
// where  _p_ ... _pe_  delimits the parameter list, _rc means
// "reference-to-const", and the trailing _const marks the method itself
// const-qualified.
//
// (Sea-front also ships an Itanium ABI mangler: --mangling=itanium produces
// _ZNK4geom3Vec3dotERKS0_ for the same method.)

namespace geom {

struct Vec {
    int x;
    int y;
    Vec(int a, int b) : x(a), y(b) {}

    // Member-function overload — same name, different parameter list.
    int scale(int k)            const { return (x + y) * k; }
    int scale(int kx, int ky)   const { return x * kx + y * ky; }

    int dot(const Vec &o)       const { return x * o.x + y * o.y; }

    // Operator overloading.
    Vec operator+(const Vec &o) const { return Vec(x + o.x, y + o.y); }
};

} // namespace geom

int main() {
    geom::Vec a(3, 4);
    geom::Vec b(1, 2);
    geom::Vec c = a + b;             // operator+
    return c.scale(2, 3) - a.dot(b);
    //   c = (4, 6); c.scale(2,3) = 4*2 + 6*3 = 26
    //   a.dot(b)                 = 3*1 + 4*2 = 11
    //   -> 15
}
