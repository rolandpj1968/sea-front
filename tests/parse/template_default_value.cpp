// Template parameter defaults exercise the proper-template-parsing path:
// - non-type defaults are parsed as constant-expressions, not skipped via
//   ad-hoc angle counting, so a relational '<' inside the default doesn't
//   confuse the parser into eating the closing '>' of the parameter list
// - type defaults are parsed as type-ids
// - the disambiguation for 'IDENT <' inside template_depth>0 must NOT
//   treat a non-type template parameter (variable) as a template-id
namespace std { template<class T> struct numeric_limits { static const int digits = 0; }; }

// Relational '<' against a non-type template parameter — was the
// libstdc++ <random> _Shift bug.
template<unsigned __w, bool = __w < 5>
struct A {};

// Relational '<' with a static_cast on the rhs — same family as above.
template<unsigned __w, bool = __w < static_cast<unsigned>(8)>
struct B {};

// Type-parameter default: a pointer type-id.
template<class T, class U = int*>
struct C {};

// Type-parameter default: a template-id (exercises the >> handling).
template<class T, class U = std::numeric_limits<T>>
struct D {};
