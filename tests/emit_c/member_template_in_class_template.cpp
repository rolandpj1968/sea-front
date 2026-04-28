// EXPECT: 3
// gcc 4.8 is_a_helper pattern:
//   template<typename T>
//   struct is_a_helper {
//       template<typename U>
//       static T *cast(U *p) { return static_cast<T *>(p); }
//   };
// Two template heads — class template AND member template.
// build_registry must descend into ND_TEMPLATE_DECL wrapping a
// class def to register the member templates with the
// instantiated class as owner.
//
// Standard: N4659 §17.5.2 [temp.mem] (a template can be declared
// within a class or class template) + §17.8.2.1 [temp.deduct.call]
// (member-template arg deduction from call args). Mangling:
// Itanium C++ ABI §5.1 (def and call must agree on encoding).

template<typename T>
struct Box {
    template<typename U>
    static int convert(U val) { return (int)val; }
};

int main() {
    return Box<int>::convert(3.14f);   // U deduced as float -> 3
}
