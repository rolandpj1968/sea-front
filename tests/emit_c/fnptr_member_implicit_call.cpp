// EXPECT: 42
// A function-pointer data member called from inside a class method
// body must lower to 'this->fn(arg)' (data-member load + indirect
// call), not a mangled-method dispatch 'Class_fn(this, arg)'. Sema
// resolves the unqualified 'fn' to the member Declaration with
// implicit-this; the call-emit path needs to distinguish a fn-ptr
// member from a method.
//
// Pattern from glibc's pthread.h __pthread_cleanup_class destructor:
//   ~__pthread_cleanup_class () {
//       if (__do_it) __cancel_routine (__cancel_arg);
//   }
// where __cancel_routine is 'void (*)(void *)' data member.

static int g_result = 0;
extern "C" void writer(void *p) {
    g_result = *(int *)p;
}

class Runner {
    void (*fn)(void *);
    void *arg;
public:
    Runner(void (*f)(void *), void *a) : fn(f), arg(a) {}
    void invoke() { fn(arg); }   // <-- function-pointer member call
};

int main() {
    int val = 42;
    Runner r(writer, &val);
    r.invoke();
    return g_result;
}
