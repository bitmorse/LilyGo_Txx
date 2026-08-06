// Unit tests for the /file/<id> URI id parser (main/fileid.c).
#include "test_framework.h"
#include "fileid.h"

static void test_valid_ids(void)
{
    CHECK(fileid_from_uri("/file/0") == 0);
    CHECK(fileid_from_uri("/file/7") == 7);
    CHECK(fileid_from_uri("/file/123") == 123);
    CHECK(fileid_from_uri("http://192.168.4.1:8080/file/42") == 42);
}

static void test_empty_segment(void)
{
    CHECK(fileid_from_uri("/file/") == -1);     // trailing slash, no id
    CHECK(fileid_from_uri("/file") == -1);      // "file" is not digits
    CHECK(fileid_from_uri("/") == -1);
}

static void test_nonnumeric(void)
{
    CHECK(fileid_from_uri("/file/abc") == -1);
    CHECK(fileid_from_uri("/file/7abc") == -1); // leading digits then junk
    CHECK(fileid_from_uri("/file/-3") == -1);   // no sign
    CHECK(fileid_from_uri("/file/ 3") == -1);   // no whitespace
    CHECK(fileid_from_uri("/file/3.0") == -1);
}

static void test_edges(void)
{
    CHECK(fileid_from_uri("") == -1);
    CHECK(fileid_from_uri(NULL) == -1);
    CHECK(fileid_from_uri("/file/99999999999") == -1);  // absurd -> overflow guard
}

int main(void)
{
    RUN(test_valid_ids);
    RUN(test_empty_segment);
    RUN(test_nonnumeric);
    RUN(test_edges);
    return REPORT();
}
