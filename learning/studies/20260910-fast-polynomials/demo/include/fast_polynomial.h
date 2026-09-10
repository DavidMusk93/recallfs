#ifndef FAST_POLYNOMIAL_H
#define FAST_POLYNOMIAL_H

#define FP_P9_DEGREE 9
#define FP_P9_HORNER_MULTIPLICATIONS 8
#define FP_P9_CHAIN_MULTIPLICATIONS 5
#define FP_P9_CHAIN_MULTIPLICATIVE_DEPTH 4

double fp_eval_p9_horner(double x);
double fp_eval_p9_chain(double x);
long double fp_eval_p9_reference(long double x);

double fp_eval_x9_horner(double x);
double fp_eval_x9_chain(double x);

#endif
