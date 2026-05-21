// EXPECT: 0
// noexcept / throw() violation triggers __sf_terminate, which calls
// std::terminate when libstdc++ is linked or abort() as the fallback
// (sea-front's runtime, weak-symbol guarded). The test harness
// runs cc without -lstdc++ so we hit the abort path.
//
// Detection: install a SIGABRT handler that exits(0). If the throw
// escaped normally instead of triggering terminate, control would
// reach main's catch and we'd return 1.
//
// Regression for 6fb895f (parse, codegen: noexcept violation calls
// __sf_terminate). I had originally deleted the regression because
// of header-include constraints; this version declares signal /
// SIGABRT manually so it works under the no-preprocessing
// test_emit_c harness.

typedef void (*sighandler_t)(int);
extern "C" sighandler_t signal(int signum, sighandler_t handler);
extern "C" void _exit(int) __attribute__((noreturn));

static void on_abort(int) { _exit(0); }

static void g() { throw 1; }
static void (*p1)() = g;
static void f() noexcept { p1(); }     // noexcept violation when p1 throws

int main() {
    signal(6 /* SIGABRT */, on_abort);
    try {
        f();
    } catch (int) {
        // Reaching the catch means the noexcept violation did NOT
        // trigger terminate — the throw escaped normally. Regression.
        return 1;
    }
    // f() returned normally — also wrong (g unconditionally throws).
    return 2;
}
