# sea-front

A C++-to-C transpiler written in C, for trusted bootstrapping of GCC and Clang.

## Goal

Build a verified path from a trusted C compiler to a production C++ compiler,
breaking the circular self-hosting dependency that modern C++ compilers have.

```
Trusted C compiler
    -> sea-front (this project: C++ to C transpiler, written in C)
    -> GCC / Clang compiled from source
    -> Production C++ toolchain, fully bootstrapped from trusted roots
```

## Status

Design phase. See the [doc/](doc/) directory for:

- [Trusted Bootstrap Design](doc/trusted-bootstrap-design.md) — architecture and design decisions
- [C++ Feature Survey](doc/cxx-feature-survey.md) — what C++ features GCC and Clang require
- [Grammar Evolution](doc/grammar-evolution.md) — C++17/20/23 grammar changes
- [Disambiguation Rules](doc/disambiguation-rules.md) — complete audit of C++ parsing ambiguities

## Approach

- **C++-to-C transpiler** (like cfront, but with whole-program analysis)
- **Hand-written recursive descent parser** (the only proven approach for C++)
- **Full C++17 grammar** with proper disambiguation
- **Pragmatic semantic subsetting** — enough to compile GCC (C++14) then Clang (C++17)
- **No exceptions, no RTTI needed** — both targets compile with `-fno-exceptions -fno-rtti`

## Building

Not yet buildable. Check back soon.

## License

MIT
