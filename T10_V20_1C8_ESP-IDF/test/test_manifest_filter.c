// Unit tests for the File Sync manifest filter (main/manifest_filter.c).
// Policy: only .mcap recordings show; hidden/dotfiles, sidecars, and every
// other extension are excluded.
#include "test_framework.h"
#include "manifest_filter.h"

static void test_mcap_included(void)
{
    CHECK(manifest_include_name("vib0001.mcap"));
    CHECK(manifest_include_name("uart0007.mcap"));
    CHECK(manifest_include_name("x.mcap"));          // minimal stem
}

static void test_case_insensitive(void)
{
    CHECK(manifest_include_name("REC.MCAP"));
    CHECK(manifest_include_name("Foo.McAp"));
}

static void test_other_extensions_excluded(void)
{
    CHECK(!manifest_include_name("clip.wav"));
    CHECK(!manifest_include_name("data.json"));
    CHECK(!manifest_include_name("song.mp3"));
    CHECK(!manifest_include_name("a.mcapx"));         // not exactly .mcap
    CHECK(!manifest_include_name("mcap"));            // no dot
    CHECK(!manifest_include_name("noext"));
}

static void test_sidecars_excluded(void)
{
    CHECK(!manifest_include_name("vib0001.mcap.s256"));
    CHECK(!manifest_include_name("vib0001.s256"));
}

static void test_hidden_excluded(void)
{
    CHECK(!manifest_include_name(".hidden.mcap"));
    CHECK(!manifest_include_name(".mcap"));           // dotfile, no stem
    CHECK(!manifest_include_name("."));
    CHECK(!manifest_include_name(".."));
}

static void test_edge_cases(void)
{
    CHECK(!manifest_include_name(""));
    CHECK(!manifest_include_name(NULL));
    CHECK(!manifest_include_name(".mcap "));          // trailing space -> no match
}

int main(void)
{
    RUN(test_mcap_included);
    RUN(test_case_insensitive);
    RUN(test_other_extensions_excluded);
    RUN(test_sidecars_excluded);
    RUN(test_hidden_excluded);
    RUN(test_edge_cases);
    return REPORT();
}
