/* Link against the dependency image's static libavutil; run in a container.
 * Behavioral regressions for CVE-2026-107676 and CVE-2026-107678. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <libavutil/encryption_info.h>
#include <libavutil/hdr_dynamic_metadata.h>
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "encryption") == 0) {
        struct rlimit limit = {128 * 1024, 128 * 1024};
        assert(setrlimit(RLIMIT_STACK, &limit) == 0);
        AVEncryptionInitInfo *head = NULL;
        for (int i = 0; i < 100000; ++i) {
            AVEncryptionInitInfo *node = av_encryption_init_info_alloc(0, 0, 0, 0);
            assert(node);
            node->next = head;
            head = node;
        }
        av_encryption_init_info_free(head);
        puts("PASS: free 100000 PSSH entries with a 128 KiB stack");
        return 0;
    }
    AVDynamicHDRPlus metadata = {0};
    metadata.num_windows = 1;
    metadata.targeted_system_display_maximum_luminance.den = 1;
    for (int i = 0; i < 3; ++i) metadata.params[0].maxscl[i].den = 1;
    metadata.params[0].average_maxrgb.den = 1;
    metadata.params[0].fraction_bright_pixels.den = 1;
    metadata.params[0].color_saturation_mapping_flag = 1;
    metadata.params[0].color_saturation_weight = (AVRational){1, 8};
    /* Output must not depend on the previous contents of the output buffer. */
    uint8_t first[AV_HDR_PLUS_MAX_PAYLOAD_SIZE], second[AV_HDR_PLUS_MAX_PAYLOAD_SIZE];
    memset(first, 0xaa, sizeof(first)); memset(second, 0x55, sizeof(second));
    uint8_t *a = first, *b = second;
    size_t a_size = sizeof(first), b_size = sizeof(second);
    assert(av_dynamic_hdr_plus_to_t35(&metadata, &a, &a_size) == 0);
    assert(av_dynamic_hdr_plus_to_t35(&metadata, &b, &b_size) == 0);
    assert(a_size == b_size && memcmp(first, second, a_size) == 0);
    AVDynamicHDRPlus decoded = {0};
    assert(av_dynamic_hdr_plus_from_t35(&decoded, first, a_size) == 0);
    assert(decoded.params[0].tone_mapping_flag == 0);
    assert(decoded.params[0].color_saturation_mapping_flag == 1);
    assert(decoded.params[0].color_saturation_weight.num == 1);
    assert(decoded.params[0].color_saturation_weight.den == 8);
    puts("PASS: deterministic HDR serialization and saturation round trip without tone mapping");
    return 0;
}
