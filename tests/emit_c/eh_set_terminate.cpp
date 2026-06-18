// EXPECT: 0
// N4659 §18.5.2 [terminate]: std::set_terminate installs a user
// handler that std::terminate must call instead of the default
// (which itself calls abort()). The handler is also invoked when
// the runtime detects a violation: throw-spec breach, throw with
// no handler, etc.
//
// Sea-front's __sf_terminate previously delegated to a weak
// _ZSt9terminatev whose only behaviour was abort(). std::
// set_terminate calls link-failed because we never emitted the
// _ZSt13set_terminatePFvvE symbol.
//
// Prelude now provides:
//   - __sf_terminate_handler — weak global, default NULL
//   - _ZSt13set_terminatePFvvE — installs handler, returns previous
//   - _ZSt13get_terminatev — accessor
//   - _ZSt9terminatev — consults handler then falls back to abort
//   - _ZSt4exiti / _ZSt5abortv — std:: forwards to libc
//
// All weak so a real libstdc++ link overrides them per ELF strong-
// beats-weak. Multi-TU duplicates dedup to one copy.

extern "C" int printf(const char *, ...);
namespace std {
    typedef void (*terminate_handler)();
    terminate_handler set_terminate(terminate_handler);
    void exit(int);
}

int counter = 0;
void my_term() { counter += 42; std::exit(0); }

void f() throw() {
    /* Violation: throw inside an empty throw-spec. Sea-front's
     * throw-spec enforcement detects the in-flight exception at the
     * function epilogue and calls __sf_terminate. */
    throw 1;
}

int main() {
    std::set_terminate(my_term);
    /* set_terminate returns the previous handler; not checked here. */
    f();
    /* Should never reach: my_term calls exit(0). */
    return 1;
}
