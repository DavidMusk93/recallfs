#include "odt.h"
#include "point_location_example.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_machine_mode(const struct odt_tree *tree) {
    double x = 0.0;
    double y = 0.0;
    int fields = 0;

    while ((fields = scanf("%lf %lf", &x, &y)) != EOF) {
        struct odt_result result = {0};
        enum odt_status status;

        if (fields != 2) {
            fputs("invalid input: expected whitespace-separated 'x y' pairs\n", stderr);
            return EXIT_FAILURE;
        }
        status = odt_locate_2d(tree, x, y, &result);
        if (status != ODT_OK) {
            fprintf(stderr, "lookup failed with status %d\n", status);
            return EXIT_FAILURE;
        }
        if (printf("%d %zu\n", result.value, result.comparisons) < 0) {
            fputs("failed to write machine output\n", stderr);
            return EXIT_FAILURE;
        }
    }
    if (ferror(stdin)) {
        fputs("failed to read machine input\n", stderr);
        return EXIT_FAILURE;
    }
    if (fflush(stdout) == EOF) {
        fputs("failed to flush machine output\n", stderr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int run_walkthrough(const struct odt_tree *tree) {
    static const double points[][2] = {
        {2.0, 2.0}, {8.0, 3.0}, {4.0, 7.0}, {5.0, 2.5}, {5.0, 5.0}, {-1.0, 4.0},
    };

    puts("sites: A=(2,2), B=(8,3), C=(4,7)");
    puts("domain: [0,10] x [0,8]");
    puts("point       region   comparisons");
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        struct odt_result result = {0};
        enum odt_status status = odt_locate_2d(tree, points[i][0], points[i][1], &result);

        if (status != ODT_OK) {
            fprintf(stderr, "lookup failed with status %d\n", status);
            return EXIT_FAILURE;
        }
        printf("(%4.1f,%4.1f) %-8s %zu\n", points[i][0], points[i][1],
               point_location_region_name(result.value), result.comparisons);
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    const struct odt_tree *tree = point_location_example_tree();
    enum odt_status status = odt_tree_validate(tree);

    if (status != ODT_OK) {
        fprintf(stderr, "example tree validation failed with status %d\n", status);
        return EXIT_FAILURE;
    }
    if (argc == 1) {
        return run_walkthrough(tree);
    }
    if (argc == 2 && strcmp(argv[1], "--machine") == 0) {
        return run_machine_mode(tree);
    }
    fprintf(stderr, "usage: %s [--machine]\n", argv[0]);
    return EXIT_FAILURE;
}
