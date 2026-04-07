// Qualified-name lookup: A::B::C — N4659 §6.4.3 [basic.lookup.qual].
// The qualified-type-name walker now uses real lookup_in_scope on
// each segment, so the resolved type is the actual nested type
// instead of the leading segment treated as opaque.

namespace ns {
    struct Outer {
        typedef int value_type;
        struct Inner {
            typedef long size_type;
        };
    };
    typedef Outer outer_alias;
}

// Namespace::Class — should resolve to Outer's actual type.
ns::Outer x;

// Namespace::Class::Nested — walks namespace then class scope.
ns::Outer::Inner y;

// Namespace::Class::typedef — walks to a typedef inside a class.
ns::Outer::value_type a;

// Namespace::Class::Nested::typedef — three-deep walk.
ns::Outer::Inner::size_type b;

// When the chain dead-ends in an unknown segment, fall back to opaque
// (last segment as tag). This documents the heuristic safety net.
ns::Outer::not_a_real_member z;
