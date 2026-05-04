// EXPECT: 54
// sizeof never decays an array — N4659 §8.3.3 [expr.sizeof]/2.
// 'sizeof(T)' where T is a typedef of an array type returns the
// total array size, not sizeof(pointer). With unsigned short[27]
// that's 27 * 2 = 54.
//
// Pattern from gcc 4.8 ira.c's XNEWVEC(move_table, N), where
// 'typedef unsigned short move_table[N_REG_CLASSES]'. Sea-front
// previously decayed the array typedef when emitting sizeof,
// returning 8 (sizeof void*) and producing a way-too-small heap
// allocation; the resulting indexed writes corrupted the heap.

typedef unsigned short row[27];

int main() {
    return (int)sizeof(row);
}
