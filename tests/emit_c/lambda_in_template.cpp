// EXPECT: 42
// Lambda inside a function template — N4659 §17.6.4 [temp.point].
// Each instantiation produces its own closure type and lambda fn
// with substituted member/return/parameter types. Parser defers
// the TU-top hoist when REGION_TEMPLATE is on the scope chain;
// the clone pass produces fresh closure + fn per instantiation
// (suffix '_instN' on the tag and fn name). The post-clone walker
// collects them into all_instantiated[] before the surrounding fn.
template<typename T>
T foo(T x) {
    auto f = [&x]() -> T { return x; };
    return f();
}
int main() { return foo<int>(42); }
