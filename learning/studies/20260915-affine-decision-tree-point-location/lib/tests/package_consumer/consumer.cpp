#include <odt.h>

#include <string.h>

using build_signature = odt_status(const odt_domain *, const odt_site *, size_t,
                                   const odt_build_options *, const odt_limits *,
                                   const odt_allocator *, odt_build_stats *, odt_generation **);
using query_signature = odt_status(const odt_generation *, const odt_point *, odt_query_result *,
                                   odt_query_stats *);
using query_batch_signature = odt_status(const odt_generation *, size_t, const void *, size_t,
                                         void *, size_t, odt_batch_stats *);
using encode_signature = odt_status(const odt_generation *, const odt_encode_options *,
                                    const odt_sink *, uint64_t *);
using load_signature = odt_status(const odt_source *, const odt_load_limits *,
                                  const odt_allocator *, odt_generation **);
using save_file_signature = odt_status(const odt_generation *, const char *,
                                       const odt_encode_options *);
using load_file_signature = odt_status(const char *, const odt_load_limits *, const odt_allocator *,
                                       odt_generation **);
using destroy_signature = void(odt_generation *);

static_assert(__cplusplus >= 201703L);
static_assert(__is_same(decltype(&odt_build), build_signature *));
static_assert(__is_same(decltype(&odt_query), query_signature *));
static_assert(__is_same(decltype(&odt_query_batch), query_batch_signature *));
static_assert(__is_same(decltype(&odt_encode), encode_signature *));
static_assert(__is_same(decltype(&odt_load), load_signature *));
static_assert(__is_same(decltype(&odt_save_file_atomic), save_file_signature *));
static_assert(__is_same(decltype(&odt_load_file), load_file_signature *));
static_assert(__is_same(decltype(&odt_generation_destroy), destroy_signature *));

int main() {
    odt_allocator allocator{};
    odt_limits limits{};
    odt_build_options build_options{};
    odt_encode_options encode_options{};
    odt_load_limits load_limits{};

    if (ODT_VERSION_MAJOR != 1 || ODT_ABI_VERSION != 1u ||
        odt_allocator_init(&allocator) != ODT_OK || odt_limits_init(&limits) != ODT_OK ||
        odt_build_options_init(&build_options) != ODT_OK ||
        odt_encode_options_init(&encode_options) != ODT_OK ||
        odt_load_limits_init(&load_limits) != ODT_OK ||
        strcmp(odt_status_string(ODT_COMMIT_UNKNOWN), "commit state unknown") != 0) {
        return 1;
    }

    void *memory = allocator.allocate(allocator.context, 17u, 16u);
    if (memory == nullptr) {
        return 2;
    }
    allocator.deallocate(allocator.context, memory, 17u, 16u);
    odt_generation_destroy(nullptr);
    return 0;
}
