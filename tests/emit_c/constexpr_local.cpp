// EXPECT: 42
// Block-scope constexpr — implicitly 'static const' under sea-front's
// lowering. The 'static' is harmless for a local that's a true
// constant: same observable value across calls, single initialisation.
int main() {
    constexpr int answer = 42;
    return answer;
}
