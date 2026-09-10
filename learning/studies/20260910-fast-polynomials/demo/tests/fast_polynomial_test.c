#include "fast_polynomial.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct exact_case {
  int x;
  long long expected;
};

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(EXIT_FAILURE);
  }
}

int main(void) {
  static const struct exact_case cases[] = {
      {-16, -4109200111LL}, {-4, 399581LL},       {-1, 12310199LL},
      {0, 34459425LL},      {1, 90960751LL},      {2, 226566107LL},
      {4, 1191200869LL},    {16, 988079200561LL},
  };
  long double max_horner_relative_error = 0.0L;
  long double max_chain_relative_error = 0.0L;

  require(FP_P9_HORNER_MULTIPLICATIONS == 8,
          "Horner multiplication-count anchor changed");
  require(FP_P9_CHAIN_MULTIPLICATIONS == 5,
          "preprocessed-chain multiplication-count anchor changed");
  require(FP_P9_CHAIN_MULTIPLICATIVE_DEPTH == 4,
          "preprocessed-chain depth anchor changed");

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    double x = (double)cases[i].x;
    double expected = (double)cases[i].expected;
    require(fp_eval_p9_horner(x) == expected,
            "Horner failed an exact integer anchor");
    require(fp_eval_p9_chain(x) == expected,
            "preprocessed chain failed an exact integer anchor");
  }

  for (int i = -4096; i <= 4096; ++i) {
    double x = (double)i / 2048.0;
    long double reference = fp_eval_p9_reference((long double)x);
    long double scale = fmaxl(1.0L, fabsl(reference));
    long double horner_error =
        fabsl((long double)fp_eval_p9_horner(x) - reference) / scale;
    long double chain_error =
        fabsl((long double)fp_eval_p9_chain(x) - reference) / scale;

    if (horner_error > max_horner_relative_error) {
      max_horner_relative_error = horner_error;
    }
    if (chain_error > max_chain_relative_error) {
      max_chain_relative_error = chain_error;
    }
  }

  require(max_horner_relative_error <= 1.0e-12L,
          "Horner exceeded the bounded-domain error tolerance");
  require(max_chain_relative_error <= 1.0e-12L,
          "preprocessed chain exceeded the bounded-domain error tolerance");

  double tiny_x = ldexp(1.0, -10);
  double tiny_reference = ldexp(1.0, -90);
  double tiny_horner = fp_eval_x9_horner(tiny_x);
  double tiny_chain = fp_eval_x9_chain(tiny_x);
  long double tiny_chain_relative =
      fabsl((long double)tiny_chain - (long double)tiny_reference) /
      (long double)tiny_reference;

  require(tiny_horner == tiny_reference,
          "x^9 Horner path should be exact at a power-of-two input");
  require(tiny_chain_relative >= 0.5L,
          "x^9 chain no longer demonstrates cancellation sensitivity");

  printf("correctness passed: 8 exact anchors, 8193-point sweep; "
         "max relative error horner=%.3Le chain=%.3Le; "
         "x^9 cancellation relative error=%.3Le\n",
         max_horner_relative_error, max_chain_relative_error,
         tiny_chain_relative);
  return EXIT_SUCCESS;
}
