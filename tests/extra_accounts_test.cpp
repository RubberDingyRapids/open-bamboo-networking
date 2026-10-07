// Hermetic unit tests for obn::accounts (obn.accounts.json parsing).

#include "obn/extra_accounts.hpp"

#include <cstdio>
#include <string>

#define EXPECT(r, cond)                                              \
    do {                                                             \
        if (cond) {                                                  \
            (r).passed++;                                            \
        } else {                                                     \
            (r).failed++;                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                \
                         __FILE__, __LINE__, #cond);                 \
        }                                                            \
    } while (0)

struct Result { int failed = 0; int passed = 0; };

void test_parse(Result& r)
{
    std::string err;
    auto list = obn::accounts::parse(R"({"version":1,"accounts":[
        {"enabled":true,"label":"Work","account":"a@work.example",
         "access_token":"tok-a","refresh_token":"ref-a",
         "expires_at":"2027-01-03T21:12:05Z","user_id":"111","user_name":"Alice"},
        {"enabled":false,"access_token":"tok-b","user_id":"222"},
        {"access_token":"tok-c","user_id":"333"}
    ]})", &err);
    EXPECT(r, err.empty());
    EXPECT(r, list.size() == 3);
    if (list.size() != 3) return;
    EXPECT(r, list[0].enabled && list[0].label == "Work");
    EXPECT(r, list[0].session.user_id == "111");
    EXPECT(r, list[0].session.access_token == "tok-a");
    EXPECT(r, list[0].session.refresh_token == "ref-a");
    EXPECT(r, list[0].session.account == "a@work.example");
    EXPECT(r, list[0].session.region == "GLOBAL");
    EXPECT(r, list[0].session.expires_at.time_since_epoch().count() != 0);
    EXPECT(r, !list[1].enabled && list[1].label.empty());
    EXPECT(r, list[2].enabled);          // enabled defaults to true
}

void test_skips_unusable_and_duplicates(Result& r)
{
    auto list = obn::accounts::parse(R"({"accounts":[
        {"user_id":"1"},
        {"access_token":"t"},
        {"user_id":"3","access_token":"first"},
        {"user_id":"3","access_token":"second"},
        "not an object"
    ]})");
    EXPECT(r, list.size() == 1);
    if (list.size() == 1) EXPECT(r, list[0].session.access_token == "first");
}

void test_bad_input(Result& r)
{
    std::string err;
    EXPECT(r, obn::accounts::parse("{\"accounts\":[", &err).empty());
    EXPECT(r, !err.empty());
    EXPECT(r, obn::accounts::parse("").empty());
    EXPECT(r, obn::accounts::parse("{}").empty());
    EXPECT(r, obn::accounts::load("/nonexistent/obn.accounts.json").empty());
}

void test_labelled_name(Result& r)
{
    EXPECT(r, obn::accounts::labelled_name("", "P1S") == "P1S");
    EXPECT(r, obn::accounts::labelled_name("Work", "P1S") == "[Work] P1S");
}

int main()
{
    Result r;
    test_parse(r);
    test_skips_unusable_and_duplicates(r);
    test_bad_input(r);
    test_labelled_name(r);
    std::printf("extra_accounts_test: %d passed, %d failed\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
