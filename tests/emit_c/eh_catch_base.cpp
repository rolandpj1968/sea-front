// EXPECT: 0
// N4659 §15.3/3 [except.handle]: a handler with parameter type
// `Base` or `Base&` matches a thrown object of type `Derived` (or
// pointer/ref-to-Derived for pointer/ref handlers). Sea-front
// formerly pointer-compared the typeinfo, so catch(Base&) never
// caught throw Derived. The runtime helper __sf_type_matches walks
// the typeinfo parent chain; class typeinfo's `parent` slot is now
// populated from the first direct non-virtual base.
//
// Limited to single inheritance — multi/virtual base catch with
// offset adjustment is a follow-on slice.

extern "C" void abort(void);

struct Base    {};
struct Derived : Base {};
struct GrandD  : Derived {};

int main() {
    /* catch(Base&) matches throw Derived. */
    {
        try { throw Derived(); }
        catch (Base&) { /* expected */ }
        catch (...)   { abort(); }
    }
    /* Walks two levels: catch(Base&) matches throw GrandD. */
    {
        try { throw GrandD(); }
        catch (Base&) { /* expected */ }
        catch (...)   { abort(); }
    }
    /* catch(Derived&) still matches throw Derived (no regression). */
    {
        try { throw Derived(); }
        catch (Derived&) { /* expected */ }
        catch (Base&)    { abort(); }
    }
    /* catch(Base&) does NOT match throw int (different chain). */
    {
        try { throw 42; }
        catch (Base&) { abort(); }
        catch (int)   { /* expected */ }
    }
    return 0;
}
