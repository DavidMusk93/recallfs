#define _POSIX_C_SOURCE 200809L

#include "fast_polynomial.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(__GNUC__) && !defined(__clang__)
#define FP_NOVECTOR __attribute__((noinline, optimize("no-tree-vectorize")))
#else
#define FP_NOVECTOR __attribute__((noinline))
#endif

static volatile double inputs[8];
static volatile double sink;
static uint64_t rng_state = UINT64_C(0x243f6a8885a308d3);

static uint64_t next_u64(void) {
  rng_state ^= rng_state << 7;
  rng_state ^= rng_state >> 9;
  return rng_state;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    perror("clock_gettime");
    exit(EXIT_FAILURE);
  }
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int compare_double(const void *left, const void *right) {
  double a = *(const double *)left;
  double b = *(const double *)right;
  return (a > b) - (a < b);
}

static double median(double *values, size_t count) {
  qsort(values, count, sizeof(values[0]), compare_double);
  return values[count / 2];
}

static void validate_inputs(void) {
  for (size_t i = 0; i < 8; ++i) {
    double x = (double)(next_u64() & UINT64_C(0xffff)) / 32768.0 - 1.0;
    double horner = fp_eval_p9_horner(x);
    double chain = fp_eval_p9_chain(x);
    long double scale = fmaxl(1.0L, fabsl((long double)horner));
    long double relative =
        fabsl((long double)chain - (long double)horner) / scale;
    if (relative > 1.0e-12L) {
      fprintf(stderr, "validation mismatch at input %zu\n", i);
      exit(EXIT_FAILURE);
    }
    inputs[i] = x;
  }
}

FP_NOVECTOR
static double measure_horner_latency(uint64_t rounds) {
  double x = inputs[0];
  uint64_t start = now_ns();

  for (uint64_t i = 0; i < rounds; ++i) {
    x = fp_eval_p9_horner(x) * 0x1p-27;
  }

  uint64_t elapsed = now_ns() - start;
  sink = x;
  return (double)elapsed / (double)rounds;
}

FP_NOVECTOR
static double measure_chain_latency(uint64_t rounds) {
  double x = inputs[0];
  uint64_t start = now_ns();

  for (uint64_t i = 0; i < rounds; ++i) {
    x = fp_eval_p9_chain(x) * 0x1p-27;
  }

  uint64_t elapsed = now_ns() - start;
  sink = x;
  return (double)elapsed / (double)rounds;
}

FP_NOVECTOR
static double measure_horner_throughput(uint64_t rounds) {
  double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
  double a4 = 0.0, a5 = 0.0, a6 = 0.0, a7 = 0.0;
  uint64_t start = now_ns();

  for (uint64_t i = 0; i < rounds; ++i) {
    a0 += fp_eval_p9_horner(inputs[0]);
    a1 += fp_eval_p9_horner(inputs[1]);
    a2 += fp_eval_p9_horner(inputs[2]);
    a3 += fp_eval_p9_horner(inputs[3]);
    a4 += fp_eval_p9_horner(inputs[4]);
    a5 += fp_eval_p9_horner(inputs[5]);
    a6 += fp_eval_p9_horner(inputs[6]);
    a7 += fp_eval_p9_horner(inputs[7]);
  }

  uint64_t elapsed = now_ns() - start;
  sink = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
  return (double)elapsed / (8.0 * (double)rounds);
}

FP_NOVECTOR
static double measure_chain_throughput(uint64_t rounds) {
  double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
  double a4 = 0.0, a5 = 0.0, a6 = 0.0, a7 = 0.0;
  uint64_t start = now_ns();

  for (uint64_t i = 0; i < rounds; ++i) {
    a0 += fp_eval_p9_chain(inputs[0]);
    a1 += fp_eval_p9_chain(inputs[1]);
    a2 += fp_eval_p9_chain(inputs[2]);
    a3 += fp_eval_p9_chain(inputs[3]);
    a4 += fp_eval_p9_chain(inputs[4]);
    a5 += fp_eval_p9_chain(inputs[5]);
    a6 += fp_eval_p9_chain(inputs[6]);
    a7 += fp_eval_p9_chain(inputs[7]);
  }

  uint64_t elapsed = now_ns() - start;
  sink = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
  return (double)elapsed / (8.0 * (double)rounds);
}

static uint64_t parse_u64(const char *text, const char *name) {
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (text[0] == '\0' || end == NULL || *end != '\0' || value == 0) {
    fprintf(stderr, "invalid %s: %s\n", name, text);
    exit(EXIT_FAILURE);
  }
  return (uint64_t)value;
}

int main(int argc, char **argv) {
  uint64_t rounds = argc > 1 ? parse_u64(argv[1], "rounds") : UINT64_C(1000000);
  size_t samples =
      argc > 2 ? (size_t)parse_u64(argv[2], "samples") : (size_t)31;

  if ((samples & 1U) == 0U || samples > 101U) {
    fprintf(stderr, "samples must be odd and no greater than 101\n");
    return EXIT_FAILURE;
  }

  double *horner_latency = calloc(samples, sizeof(*horner_latency));
  double *chain_latency = calloc(samples, sizeof(*chain_latency));
  double *horner_throughput = calloc(samples, sizeof(*horner_throughput));
  double *chain_throughput = calloc(samples, sizeof(*chain_throughput));
  if (horner_latency == NULL || chain_latency == NULL ||
      horner_throughput == NULL || chain_throughput == NULL) {
    fprintf(stderr, "allocation failed\n");
    free(horner_latency);
    free(chain_latency);
    free(horner_throughput);
    free(chain_throughput);
    return EXIT_FAILURE;
  }

  validate_inputs();
  (void)measure_horner_latency(1024);
  (void)measure_chain_latency(1024);
  (void)measure_horner_throughput(1024);
  (void)measure_chain_throughput(1024);

  for (size_t sample = 0; sample < samples; ++sample) {
    if ((sample & 1U) == 0U) {
      horner_latency[sample] = measure_horner_latency(rounds);
      chain_latency[sample] = measure_chain_latency(rounds);
      horner_throughput[sample] = measure_horner_throughput(rounds);
      chain_throughput[sample] = measure_chain_throughput(rounds);
    } else {
      chain_latency[sample] = measure_chain_latency(rounds);
      horner_latency[sample] = measure_horner_latency(rounds);
      chain_throughput[sample] = measure_chain_throughput(rounds);
      horner_throughput[sample] = measure_horner_throughput(rounds);
    }
  }

  printf("mode,method,ns_per_eval,rounds,samples\n");
  printf("latency,horner,%.3f,%llu,%zu\n", median(horner_latency, samples),
         (unsigned long long)rounds, samples);
  printf("latency,preprocessed_chain,%.3f,%llu,%zu\n",
         median(chain_latency, samples), (unsigned long long)rounds, samples);
  printf("throughput,horner,%.3f,%llu,%zu\n",
         median(horner_throughput, samples), (unsigned long long)rounds,
         samples);
  printf("throughput,preprocessed_chain,%.3f,%llu,%zu\n",
         median(chain_throughput, samples), (unsigned long long)rounds,
         samples);
  printf("sink=%.17g\n", sink);

  free(horner_latency);
  free(chain_latency);
  free(horner_throughput);
  free(chain_throughput);
  return EXIT_SUCCESS;
}
