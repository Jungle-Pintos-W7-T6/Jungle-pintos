#ifndef THREAD_FIXED_POINT_H
#define THREAD_FIXED_POINT_H

#define F (1 << 14)

#define REAL_TO_FIXED(x) ((long long)(x) * F)
#define FIXED_TO_REAL_DOWN(x) ((long long)(x) / F)
#define FIXED_TO_REAL_ROUND(x) ((long long)(x) > 0 ? (((long long)(x) + F / 2) / F) : (((long long)(x) - F / 2) / F))

#define FIXED_ADD(x, y) ((long long)(x) + (long long)(y))
#define FIXED_SUB(x, y) ((long long)(x) - (long long)(y))
#define FIXED_MUL(x, y) (((long long)(x) * (long long)(y)) / F)
#define FIXED_DIV(x, y) (((long long)(x) * F) / (long long)(y))
#endif