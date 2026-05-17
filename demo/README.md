# sea-front — output samples

Five small C++ files, each paired with its sea-front `.c` output. Generated
with human-readable mangling so the names stay speakable.

Reproduce any one with:

    sea-front --emit-c --no-lines --mangling=human FILE.cpp > FILE.c

(`--mangling=itanium` produces ABI-mangled names like `_ZNK4geom3Vec3dotERKS0_`.)

| File              | Feature                                                      |
|-------------------|--------------------------------------------------------------|
| `01_mangling`     | namespaces, methods, member overloads, operator overloading  |
| `02_ctor_dtor`    | RAII, nested scopes, the `__SF_cleanup_N` goto-chain         |
| `03_vtable`       | per-class vtable struct + instance, virtual dispatch via vptr |
| `04_templates`    | class & function templates, instantiation, dedup, mangling   |
| `05_exceptions`   | TLS-polling EH (see `docs/exceptions.md`)                    |

## Known cosmetic caveats

Two things show up as `-Wincompatible-pointer-types` warnings under `gcc -Wall`
in `03_vtable.c` — programs run correctly but the C isn't strictly conforming
on these two lowerings yet:

  1. **Derived → base pointer conversion.** C++ implicit conversion (`Cat*` →
     `Animal*`) is lowered as a plain assignment; a `(struct sf__Animal *)`
     cast is needed for strict C.
  2. **Vtable-instance assignment in derived ctor.** The derived class's vtable
     instance has a different struct type from the polymorphic root's vptr
     field; needs a `(const struct sf__Animal__vtable *)` cast.

Both are tracked. The Itanium ABI lowering plans to fold these into the
existing vptr-routing helpers.
