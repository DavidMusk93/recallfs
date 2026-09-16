#include <odt.h>

#include <string.h>

typedef odt_status odt_build_signature(const odt_domain *, const odt_site *, size_t,
                                       const odt_build_options *, const odt_limits *,
                                       const odt_allocator *, odt_build_stats *, odt_generation **);
typedef odt_status odt_query_signature(const odt_generation *, const odt_point *,
                                       odt_query_result *, odt_query_stats *);
typedef odt_status odt_query_batch_signature(const odt_generation *, size_t, const void *, size_t,
                                             void *, size_t, odt_batch_stats *);
typedef odt_status odt_encode_signature(const odt_generation *, const odt_encode_options *,
                                        const odt_sink *, uint64_t *);
typedef odt_status odt_load_signature(const odt_source *, const odt_load_limits *,
                                      const odt_allocator *, odt_generation **);
typedef odt_status odt_save_file_signature(const odt_generation *, const char *,
                                           const odt_encode_options *);
typedef odt_status odt_load_file_signature(const char *, const odt_load_limits *,
                                           const odt_allocator *, odt_generation **);
typedef void odt_destroy_signature(odt_generation *);

#define ODT_EXPECT_FUNCTION(name, signature)                                                       \
    _Static_assert(_Generic(&(name), signature *: 1, default: 0), #name " signature changed")

ODT_EXPECT_FUNCTION(odt_build, odt_build_signature);
ODT_EXPECT_FUNCTION(odt_query, odt_query_signature);
ODT_EXPECT_FUNCTION(odt_query_batch, odt_query_batch_signature);
ODT_EXPECT_FUNCTION(odt_encode, odt_encode_signature);
ODT_EXPECT_FUNCTION(odt_load, odt_load_signature);
ODT_EXPECT_FUNCTION(odt_save_file_atomic, odt_save_file_signature);
ODT_EXPECT_FUNCTION(odt_load_file, odt_load_file_signature);
ODT_EXPECT_FUNCTION(odt_generation_destroy, odt_destroy_signature);

int main(void) {
    const odt_domain domain = {0.0, 0.0, 12.0, 8.0};
    const odt_site sites[] = {
        {0.0, 0.0, 11},
        {2.0, 0.0, 22},
        {1.0, 2.0, 33},
    };
    const odt_point inside = {1.0, 0.0};
    const odt_point outside = {13.0, 0.0};
    odt_allocator allocator;
    odt_limits limits;
    odt_build_options build_options;
    odt_encode_options encode_options;
    odt_load_limits load_limits;
    odt_generation *generation = NULL;
    odt_query_result result;
    void *memory;

    if (ODT_VERSION_MAJOR != 1 || ODT_ABI_VERSION != 1u ||
        odt_allocator_init(&allocator) != ODT_OK || odt_limits_init(&limits) != ODT_OK ||
        odt_build_options_init(&build_options) != ODT_OK ||
        odt_encode_options_init(&encode_options) != ODT_OK ||
        odt_load_limits_init(&load_limits) != ODT_OK ||
        strcmp(odt_status_string(ODT_COMMIT_UNKNOWN), "commit state unknown") != 0) {
        return 1;
    }

    memory = allocator.allocate(allocator.context, 17u, 16u);
    if (memory == NULL) {
        return 2;
    }
    allocator.deallocate(allocator.context, memory, 17u, 16u);

    if (odt_build(&domain, sites, sizeof(sites) / sizeof(sites[0]), NULL, NULL, NULL, NULL,
                  &generation) != ODT_OK ||
        odt_query(generation, &inside, &result, NULL) != ODT_OK ||
        result.kind != ODT_RESULT_REGION || result.region_id != 11 ||
        odt_query(generation, &outside, &result, NULL) != ODT_OK ||
        result.kind != ODT_RESULT_OUTSIDE || result.region_id != 0) {
        odt_generation_destroy(generation);
        return 3;
    }
    odt_generation_destroy(generation);
    return 0;
}
