// EXPECT: 0
// N4659 §18.6.4 [uncaught.exceptions]: std::uncaught_exception()
// returns true when an exception is in flight (after throw, before
// the matching handler fully matches). Deprecated in C++17, removed
// in C++20, but ubiquitous in pre-C++17 code — especially destructor
// bodies that want to know whether they're being called as part of
// unwind cleanup.
//
// std::uncaught_exceptions() (C++17, plural) returns a count rather
// than a bool. Sea-front doesn't track nested counts; we return 1
// when in flight, 0 otherwise — accurate for the single-throw case
// which is the only one most code cares about.
//
// Sea-front already tracks the in-flight bit as
// __sf_exc_state.state == __SF_UNWIND_THROW; both std:: forms now
// just read that field. Mangles:
//   _ZSt18uncaught_exceptionv   — bool std::uncaught_exception()
//   _ZSt19uncaught_exceptionsv  — int  std::uncaught_exceptions()

extern "C" void abort();
namespace std {
    bool uncaught_exception();
    int  uncaught_exceptions();
}

int dtor_under_unwind = 0;
int dtor_normal = 0;

struct Tracker {
    ~Tracker() {
        if (std::uncaught_exception()) ++dtor_under_unwind;
        else                            ++dtor_normal;
    }
};

void may_throw(bool t) { if (t) throw 1; }

int main() {
    /* Normal exit — uncaught_exception should be false. */
    { Tracker t; }
    if (dtor_normal != 1) abort();
    if (dtor_under_unwind != 0) abort();

    /* Throw — Tracker dtor runs during unwind. */
    try {
        Tracker t;
        may_throw(true);
    } catch (int) {}
    if (dtor_under_unwind != 1) abort();
    if (dtor_normal != 1) abort();

    /* uncaught_exceptions() reads the same state. */
    if (std::uncaught_exceptions() != 0) abort();

    return 0;
}
