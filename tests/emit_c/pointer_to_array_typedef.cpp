// EXPECT: 7
// Pointer-to-array via an array typedef. C declarator syntax must
// preserve the inner array bound:
//
//   typedef unsigned short row[N];
//   row *p;          → 'unsigned short (*p)[N]'
//   row *arr[M];     → 'unsigned short (*arr[M])[N]'
//
// Without the inner '[N]' the indexed addressing arr[i][j][k] computes
// the wrong byte offset (collapses to T**). Pattern from gcc 4.8
// ira-int.h:
//   typedef unsigned short move_table[N_REG_CLASSES];
//   move_table *x_ira_register_move_cost[MAX_MACHINE_MODE];
// indexed as [mode][cl1][cl2].

typedef unsigned short row[5];

row *table[3];     // array of 3 pointers, each → row (= 5 ushorts)

int main() {
    row r0 = {0, 1, 2, 3, 4};
    row r1 = {10, 11, 12, 13, 14};
    row r2 = {20, 21, 22, 23, 24};
    table[0] = &r0;
    table[1] = &r1;
    table[2] = &r2;

    // table[2][0][2] = (*table[2])[2] = r2[2] = 22.
    // Under the buggy 'unsigned short**' interpretation this would
    // dereference table[2] (pointer to array) as a pointer-to-pointer,
    // landing on garbage.
    return (int)((*table[2])[2]) - 15;     // 22 - 15 = 7
}
