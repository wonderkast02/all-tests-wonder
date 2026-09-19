// SPDX-License-Identifier: MIT
#include "common.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int expect_string(const char *actual, const char *expected, int code) {
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "expected '%s', got '%s'\n", expected, actual);
        return code;
    }
    return 0;
}

int main(void) {
    char key[128];
    char escaped[128];
    int rc;

    dgl_sanitize_key("Grand Theft Auto V.exe", key, sizeof(key));
    rc = expect_string(key, "grand-theft-auto-v", 1); if (rc) return rc;
    dgl_sanitize_key("Cyberpunk2077.EXE", key, sizeof(key));
    rc = expect_string(key, "cyberpunk2077", 2); if (rc) return rc;
    dgl_sanitize_key("  ", key, sizeof(key));
    rc = expect_string(key, "unknown-game", 3); if (rc) return rc;

    dgl_json_escape("a\"b\\c\n", escaped, sizeof(escaped));
    rc = expect_string(escaped, "a\\\"b\\\\c\\n", 4); if (rc) return rc;

    if (!dgl_contains_ci("VKD3D-Proton 2.14", "proton")) return 5;
    if (dgl_contains_ci("PanVK", "dxvk")) return 6;

    {
        static const unsigned char abc[] = "abc";
        static const char expected[] = "ba7816bf8f01cfea414140de5dae2223"
                                       "b00361a396177a9cb410ff61f20015ad";
        dgl_sha256_ctx ctx;
        unsigned char digest[32];
        char hex[65];
        size_t i;
        dgl_sha256_init(&ctx);
        dgl_sha256_update(&ctx, abc, 3);
        dgl_sha256_final(&ctx, digest);
        for (i = 0; i < 32; ++i) (void)snprintf(hex + i * 2, 3, "%02x", digest[i]);
        hex[64] = '\0';
        if (strcmp(hex, expected) != 0) return 7;
    }

    puts("test_common: PASS");
    return 0;
}
