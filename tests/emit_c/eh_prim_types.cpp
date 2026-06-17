// EXPECT: 0
// Per-primitive typeinfo for catch dispatch — N4659 §15.3
// [except.handle]. Sea-front formerly used a single
// `__sf_typeinfo_int` for every primitive throw, so `throw long_var`
// matched `catch(int)`. Now each primitive kind gets its own
// typeinfo slot and the catch handler pointer-compares against the
// matching one.

extern "C" void abort(void);

int main() {
    /* long doesn't match int / bool, lands at the long handler. */
    {
        long x = 42;
        try { throw x; }
        catch (int)  { abort(); }
        catch (bool) { abort(); }
        catch (long) { /* expected */ }
        catch (...)  { abort(); }
    }
    /* unsigned int doesn't match signed int. */
    {
        unsigned int u = 7;
        try { throw u; }
        catch (int)         { abort(); }
        catch (unsigned int) { /* expected */ }
        catch (...)         { abort(); }
    }
    /* char doesn't match int. */
    {
        char c = 'a';
        try { throw c; }
        catch (int)  { abort(); }
        catch (char) { /* expected */ }
        catch (...)  { abort(); }
    }
    /* int still works (no regression). */
    {
        try { throw 5; }
        catch (long) { abort(); }
        catch (int)  { /* expected */ }
        catch (...)  { abort(); }
    }
    return 0;
}
