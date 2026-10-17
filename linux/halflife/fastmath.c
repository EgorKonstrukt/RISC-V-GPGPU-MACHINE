// Sine and cosine in single precision, in place of musl's (which work in double, which this machine
// emulates): the angle less a whole number of quarter turns (Cody-Waite), then Cephes' polynomials.
// Good to a few units in the last place for the angles a game has (well under 10^5).

static inline float reduce(float x, int* quarter) {
    int n = (int)(x * 0.636619772f + (x >= 0 ? 0.5f : -0.5f));
    float f = (float)n;
    *quarter = n;
    return ((x - f * 1.5703125f) - f * 4.837512969970703125e-4f) - f * 7.54978995489188216e-8f;
}
static inline float sin_poly(float r) {
    float z = r * r;
    return r + r * z * (-1.6666654611e-1f + z * (8.3321608736e-3f + z * -1.9515295891e-4f));
}
static inline float cos_poly(float r) {
    float z = r * r;
    return 1.0f - 0.5f * z + z * z * (4.166664568298827e-2f + z * (-1.388731625493765e-3f + z * 2.443315711809948e-5f));
}

float sinf(float x) {
    int q;
    float r = reduce(x, &q);
    switch (q & 3) {
        case 0: return sin_poly(r);
        case 1: return cos_poly(r);
        case 2: return -sin_poly(r);
        default: return -cos_poly(r);
    }
}
float cosf(float x) {
    int q;
    float r = reduce(x, &q);
    switch (q & 3) {
        case 0: return cos_poly(r);
        case 1: return -sin_poly(r);
        case 2: return -cos_poly(r);
        default: return sin_poly(r);
    }
}
void sincosf(float x, float* s, float* c) {
    int q;
    float r = reduce(x, &q), sn = sin_poly(r), cs = cos_poly(r);
    switch (q & 3) {
        case 0: *s = sn, *c = cs; break;
        case 1: *s = cs, *c = -sn; break;
        case 2: *s = -sn, *c = -cs; break;
        default: *s = -cs, *c = sn; break;
    }
}
