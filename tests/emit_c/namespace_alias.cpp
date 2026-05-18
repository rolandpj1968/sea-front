// EXPECT: 0
// namespace-alias-definition — N4659 §10.3.2 [namespace.alias]
//   namespace identifier = qualified-namespace-specifier ;
// libstdc++ cxxabi.h uses 'namespace abi = __cxxabiv1;' at top scope;
// without the parse arm, every test transitively including cxxabi.h
// failed at the parser.

namespace target {
    int v = 7;
}

namespace alias_name = target;

int main() {
    return alias_name::v - 7;
}
