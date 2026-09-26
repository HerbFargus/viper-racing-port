// phys_math.cpp -- M3 3.2: the physics library's out-of-line maths helpers, rewritten.
//
// The 1998 compiler inlined most vector maths into its callers; these are the copies it left as
// functions in the physics object files. Each is written from its v1.0 disassembly: the same sums in the
// same grouping, the same float constants, 1/x computed once and multiplied where the original does.
// Divisions in the original go through the Pentium FDIV-bug workaround (`__adjust_fdiv ? __adj_fdiv_r :
// fdiv`), which is a plain fdiv on any CPU without that bug, so they're plain divisions here.
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// each writes only its output (and reads its inputs), so its footprint is the output and it's pure
#define PURE_OUT(OUT, BYTES) { f.add((void*)(OUT), (BYTES), "out"); f.pure = true; }

// VectorLength (phystask.obj 0x429100): sqrt((y*y + z*z) + x*x), returned unrounded in ST0
static double __cdecl VectorLength(const P3* v) {
    return x87_sqrt(((double)v->y * v->y + (double)v->z * v->z) + (double)v->x * v->x);
}
static void fp_vector_length(Footprint& f, const P3*) { f.pure = true; }
PORT_FN(0x00429100, "VectorLength", VectorLength, fp_vector_length)

// VectorSub (phystask.obj 0x429120)
static void __cdecl VectorSub(P3* o, const P3* a, const P3* b) {
    o->x = a->x - b->x;
    o->y = a->y - b->y;
    o->z = a->z - b->z;
}
static void fp_vector_sub(Footprint& f, P3* o, const P3*, const P3*) PURE_OUT(o, 12)
PORT_FN(0x00429120, "VectorSub", VectorSub, fp_vector_sub)

// VectorAdd (car.obj 0x43b0f0): the z sum is a + b, the others b + a (the same bits either way: + is
// commutative in IEEE arithmetic; only the grouping of three or more terms matters)
static void __cdecl VectorAdd(P3* o, const P3* a, const P3* b) {
    o->x = b->x + a->x;
    o->y = b->y + a->y;
    o->z = a->z + b->z;
}
static void fp_vector_add(Footprint& f, P3* o, const P3*, const P3*) PURE_OUT(o, 12)
PORT_FN(0x0043b0f0, "VectorAdd", VectorAdd, fp_vector_add)

// CrossProduct (car.obj 0x43b120): o = a x b, safe when o is a or b
static void __cdecl CrossProduct(P3* o, const P3* a, const P3* b) {
    float x = (float)(D(a->y) * b->z - D(b->y) * a->z);
    float y = (float)(D(b->x) * a->z - D(a->x) * b->z);
    float z = (float)(D(a->x) * b->y - D(b->x) * a->y);
    o->x = x;
    o->y = y;
    o->z = z;
}
static void fp_cross(Footprint& f, P3* o, const P3*, const P3*) PURE_OUT(o, 12)
PORT_FN(0x0043b120, "CrossProduct", CrossProduct, fp_cross)

// MatrixMakeIdentity (phystask.obj 0x429150)
static void __cdecl MatrixMakeIdentity(M3* m) {
    m->m[0] = 1.0f; m->m[1] = 0.0f; m->m[2] = 0.0f;
    m->m[3] = 0.0f; m->m[4] = 1.0f; m->m[5] = 0.0f;
    m->m[6] = 0.0f; m->m[7] = 0.0f; m->m[8] = 1.0f;
}
static void fp_identity(Footprint& f, M3* m) PURE_OUT(m, 36)
PORT_FN(0x00429150, "MatrixMakeIdentity", MatrixMakeIdentity, fp_identity)

// MatrixMulPoint (phystask.obj 0x429420): o = p x M (rows weighted by p), safe when o is p
static void __cdecl MatrixMulPoint(P3* o, const P3* p, const M3* m) {
    const float* M = m->m;
    float x = (float)((D(M[6]) * p->z + D(M[3]) * p->y) + D(M[0]) * p->x);
    float y = (float)((D(M[1]) * p->x + D(M[7]) * p->z) + D(M[4]) * p->y);
    float z = (float)((D(M[5]) * p->y + D(M[2]) * p->x) + D(M[8]) * p->z);
    o->x = x;
    o->y = y;
    o->z = z;
}
static void fp_mul_point(Footprint& f, P3* o, const P3*, const M3*) PURE_OUT(o, 12)
PORT_FN(0x00429420, "MatrixMulPoint", MatrixMulPoint, fp_mul_point)

// MatrixMulPointInv (volume.obj 0x436120): o = M p (the rows dotted with p: the inverse of a rotation),
// safe when o is p
static void __cdecl MatrixMulPointInv(P3* o, const P3* p, const M3* m) {
    const float* M = m->m;
    float x = (float)((D(M[1]) * p->y + D(M[2]) * p->z) + D(p->x) * M[0]);
    float y = (float)((D(M[5]) * p->z + D(M[3]) * p->x) + D(M[4]) * p->y);
    float z = (float)((D(M[6]) * p->x + D(M[7]) * p->y) + D(M[8]) * p->z);
    o->x = x;
    o->y = y;
    o->z = z;
}
static void fp_mul_point_inv(Footprint& f, P3* o, const P3*, const M3*) PURE_OUT(o, 12)
PORT_FN(0x00436120, "MatrixMulPointInv", MatrixMulPointInv, fp_mul_point_inv)

// MatrixNormalize (phystask.obj 0x429180): re-orthonormalise a rotation. Row 0 is kept (normalised); row 1
// is rebuilt as row 2 x row 0, normalised; row 2 as row 0 x row 1.
static void __cdecl MatrixNormalize(M3* m) {
    float r0x = m->m[0], r0y = m->m[1], r0z = m->m[2];
    float r2x = m->m[6], r2y = m->m[7], r2z = m->m[8];
    float r1x = (float)(D(r2y) * r0z - D(r2z) * r0y);
    float r1y = (float)(D(r2z) * r0x - D(r2x) * r0z);
    float r1z = (float)(D(r2x) * r0y - D(r2y) * r0x);
    // the lengths and their reciprocals stay in registers; the rows are stored
    double k = 1.0f / x87_sqrt(((double)r0y * r0y + (double)r0z * r0z) + (double)r0x * r0x);
    r0x = (float)(r0x * k);
    r0y = (float)(r0y * k);
    r0z = (float)(k * r0z);
    k = 1.0f / x87_sqrt(((double)r1y * r1y + (double)r1z * r1z) + (double)r1x * r1x);
    r1x = (float)(r1x * k);
    r1y = (float)(r1y * k);
    r1z = (float)(k * r1z);
    r2x = (float)(D(r1z) * r0y - D(r1y) * r0z);
    r2y = (float)(D(r1x) * r0z - D(r1z) * r0x);
    r2z = (float)(D(r1y) * r0x - D(r1x) * r0y);
    m->m[0] = r0x; m->m[1] = r0y; m->m[2] = r0z;
    m->m[3] = r1x; m->m[4] = r1y; m->m[5] = r1z;
    m->m[6] = r2x; m->m[7] = r2y; m->m[8] = r2z;
}
static void fp_normalize(Footprint& f, M3* m) PURE_OUT(m, 36)
PORT_FN(0x00429180, "MatrixNormalize", MatrixNormalize, fp_normalize)

// MatrixSet (aero.obj 0x43b3d0): rows from three vectors. The original copies with integer moves (as do
// these: a float copy through the FPU would quieten a signalling NaN, changing its bits).
static void __cdecl MatrixSetRows(M3* m, const P3* a, const P3* b, const P3* c) {
    memcpy(&m->m[0], a, 12);
    memcpy(&m->m[3], b, 12);
    memcpy(&m->m[6], c, 12);
}
static void fp_set_rows(Footprint& f, M3* m, const P3*, const P3*, const P3*) PURE_OUT(m, 36)
PORT_FN(0x0043b3d0, "MatrixSet(rows)", MatrixSetRows, fp_set_rows)

// MatrixSet (aero.obj 0x43b610): nine floats
static void __cdecl MatrixSetFloats(M3* m, const float* v) {
    memmove(m->m, v, 36);                               // rep movsd
}
static void fp_set_floats(Footprint& f, M3* m, const float*) PURE_OUT(m, 36)
PORT_FN(0x0043b610, "MatrixSet(floats)", MatrixSetFloats, fp_set_floats)

// MatrixInverse (aero.obj 0x43b420), for m = [a b c; d e f; g h i]: the adjugate over the determinant,
// det = (h t1 + g t0) + t2 i with t = row 0 x row 1. Safe when the output is the input.
static void __cdecl MatrixInverse(M3* o, const M3* m) {
    float a = m->m[0], b = m->m[1], c = m->m[2];
    float d = m->m[3], e = m->m[4], f = m->m[5];
    float g = m->m[6], h = m->m[7], i = m->m[8];
    float t0 = (float)((double)f * b - (double)e * c);          // stored
    float t1 = (float)((double)c * d - (double)f * a);
    float t2 = (float)((double)e * a - (double)d * b);
    double det = ((double)h * t1 + (double)g * t0) + (double)t2 * i;   // kept in a register
    float r[9];
    r[0] = (float)(((double)i * e - (double)h * f) / det);
    r[1] = (float)(((double)h * c - (double)i * b) / det);
    r[2] = (float)(t0 / det);
    r[3] = (float)(((double)g * f - (double)i * d) / det);
    r[4] = (float)(((double)i * a - (double)g * c) / det);
    r[5] = (float)(t1 / det);
    r[6] = (float)(((double)d * h - (double)e * g) / det);
    r[7] = (float)(((double)b * g - (double)h * a) / det);
    r[8] = (float)(t2 / det);
    for (int k = 0; k < 9; k++) o->m[k] = r[k];
}
static void fp_inverse(Footprint& f, M3* o, const M3*) PURE_OUT(o, 36)
PORT_FN(0x0043b420, "MatrixInverse", MatrixInverse, fp_inverse)

// MatrixToQuat (phystask.obj 0x4294c0): the standard conversion, largest-diagonal branch when the trace
// isn't positive
static void __cdecl MatrixToQuat(Q4* q, const M3* mm) {
    const float* m = mm->m;
    float* Q = &q->x;
    float trace = (float)((D(m[0]) + m[8]) + m[4]);
    if (trace > 0.0f) {
        double s = x87_sqrt(D(trace) + 1.0f);
        q->w = (float)(0.5f * s);
        double k = 0.5f / s;
        q->x = (float)((D(m[5]) - m[7]) * k);
        q->y = (float)((D(m[6]) - m[2]) * k);
        q->z = (float)((D(m[1]) - m[3]) * k);
        return;
    }
    // fcomp; test ah,1 (C0): "less, or unordered" -- true for a NaN, unlike C's <
    int i = !(m[0] >= m[4]) ? 1 : 0;
    if (!(m[i * 4] >= m[8])) i = 2;
    int j = (i + 1) % 3, k = (j + 1) % 3;
    double s = x87_sqrt((D(m[i * 4]) - (D(m[k * 4]) + m[j * 4])) + 1.0f);
    Q[i] = (float)(0.5f * s);
    double r = 0.5f / s;
    q->w = (float)((D(m[j * 3 + k]) - m[k * 3 + j]) * r);
    Q[j] = (float)((D(m[i * 3 + j]) + m[j * 3 + i]) * r);
    Q[k] = (float)((D(m[k * 3 + i]) + m[i * 3 + k]) * r);
}
static void fp_to_quat(Footprint& f, Q4* q, const M3*) PURE_OUT(q, 16)
PORT_FN(0x004294c0, "MatrixToQuat", MatrixToQuat, fp_to_quat)

// MatrixMakeModelRotation (phystask.obj 0x429350): M = Ry(a); then M = Rx(b) M; then M = Rz(c) M, through
// the engine's MatrixConcat (root, not yet rewritten)
typedef void(__cdecl* MatrixConcat_t)(M3*, const M3*, const M3*);
static void __cdecl MatrixMakeModelRotation(M3* m, float a, float b, float c) {
    const MatrixConcat_t concat = (MatrixConcat_t)0x00403040;
    double ca = x87_cos(a), sa = x87_sin(a);
    m->m[1] = 0.0f; m->m[3] = 0.0f; m->m[4] = 1.0f; m->m[5] = 0.0f; m->m[7] = 0.0f;
    m->m[0] = (float)ca; m->m[2] = (float)-sa; m->m[6] = (float)sa; m->m[8] = (float)ca;
    M3 r;
    double cb = x87_cos(b), sb = x87_sin(b);
    r.m[0] = 1.0f; r.m[1] = 0.0f; r.m[2] = 0.0f; r.m[3] = 0.0f; r.m[6] = 0.0f;
    r.m[4] = (float)cb; r.m[5] = (float)sb; r.m[7] = (float)-sb; r.m[8] = (float)cb;
    concat(m, &r, m);
    double cc = x87_cos(c), sc = x87_sin(c);
    r.m[2] = 0.0f; r.m[5] = 0.0f; r.m[6] = 0.0f; r.m[7] = 0.0f; r.m[8] = 1.0f;
    r.m[0] = (float)cc; r.m[1] = (float)sc; r.m[3] = (float)-sc; r.m[4] = (float)cc;
    concat(m, &r, m);
}
static void fp_model_rotation(Footprint& f, M3* m, float, float, float) PURE_OUT(m, 36)
PORT_FN(0x00429350, "MatrixMakeModelRotation", MatrixMakeModelRotation, fp_model_rotation)
