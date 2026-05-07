// EXPECT: 7
// C++20 [[likely]] / [[unlikely]] attribute applied to case labels —
// N4659 (subseq P0479R5 in C++20) [dcl.attr.likelihood]: branch
// hints, no semantic effect on program behavior. Sea-front skips
// all C++ attributes at statement position regardless of dialect;
// see project_std_authenticity for why we don't gate on --std=.
//
// Real-world hit: gcc 14's libcpp/lex.cc uses 'ATTR_LIKELY case'
// expanded from a system.h macro that's gated on
// __has_cpp_attribute(likely). g++'s preprocessor recognises the
// attribute as an extension regardless of -std=, so the form
// reaches sea-front in our preprocessing pipeline.

int classify(int n) {
    switch (n) {
        [[unlikely]] case 0:
            return 0;
        [[likely]] case 7:
            return 7;
        default:
            return -1;
    }
}

int main() {
    return classify(7);
}
