// EXPECT: 60
// Generalized pointer-to-array shapes — N4659 §11.3 [dcl.meaning]:
//   T (*p)[N]                    — pointer to array
//   T (**pp)[N]                  — pointer to pointer to array
//   T (*func())[N]               — function returning pointer-to-array
//   T (*p)[N], passed by param   — array bound visible to callee
// All share the property that the inner array bound must survive the
// declarator emit; the prior unconditional decay to '**' produced
// invalid C and / or wrong byte arithmetic.

typedef int row[5];

// Return-type variant: function returns 'int (*)[5]'.
row *get_row(row *base) { return base; }

// Param-type variant: callee receives 'int (*)[5]', sums its 5 ints.
int sum_row(row *p) {
    int s = 0;
    for (int i = 0; i < 5; i++) s += (*p)[i];
    return s;
}

// Pointer-to-pointer-to-array: 'int (**)[5]'.
int sum_via_pp(row **pp) {
    return sum_row(*pp);
}

int main() {
    row data = {2, 4, 6, 8, 10};       // sum = 30
    row *p = get_row(&data);
    int s1 = sum_row(p);                 // 30
    int s2 = sum_via_pp(&p);             // 30
    return s1 + s2;                      // 60
}
