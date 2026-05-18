// EXPECT: 42
// ANON_REWRITE_STACK_MAX = 32 used to silently drop anonymous-union
// frames past the 32nd. Member references in the enclosing scope
// then mis-resolved (bare ident lookup instead of qualified rewrite
// through the synth instance). Stack is now malloc-grown.
//
// 40 anonymous unions in one block — past the previous cap.

int main() {
    union { int u0;  }; union { int u1;  }; union { int u2;  }; union { int u3;  };
    union { int u4;  }; union { int u5;  }; union { int u6;  }; union { int u7;  };
    union { int u8;  }; union { int u9;  }; union { int u10; }; union { int u11; };
    union { int u12; }; union { int u13; }; union { int u14; }; union { int u15; };
    union { int u16; }; union { int u17; }; union { int u18; }; union { int u19; };
    union { int u20; }; union { int u21; }; union { int u22; }; union { int u23; };
    union { int u24; }; union { int u25; }; union { int u26; }; union { int u27; };
    union { int u28; }; union { int u29; }; union { int u30; }; union { int u31; };
    union { int u32; }; union { int u33; }; union { int u34; }; union { int u35; };
    union { int u36; }; union { int u37; }; union { int u38; }; union { int u39; };
    u35 = 42;  /* 36th anon union — past the previous 32-cap. */
    return u35;
}
