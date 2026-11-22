// tests_semantics.cpp
//
// SET / GET / DEL / EXPIRE / PERSIST / TTL 的语义，以及惰性删除在读写路径上的表现。

#include <optional>
#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_semantics() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    check(!s.get("missing").has_value(), "GET of a missing key");
    check(s.pttl("missing") == -2, "PTTL of a missing key");
    check(s.ttl("missing") == -2, "TTL of a missing key");

    check(s.set("k", "v0"), "SET must succeed");
    check(s.pttl("k") == -1, "a new key must have no TTL");
    check(s.expire_in("k", 5000) == 1, "EXPIRE must return 1");
    check(s.pttl("k") == 5000, "PTTL right after EXPIRE");
    check(s.ttl("k") == 5, "TTL rounds to 5 seconds");

    now += 1000;
    check(s.pttl("k") == 4000, "PTTL must count down");
    check(s.ttl("k") == 4, "TTL must count down");

    check(s.expire_in("k", 9000, ExpireFlags::Nx) == 0,
          "NX must not overwrite an existing TTL");
    check(s.expire_in("k", 9000, ExpireFlags::Xx) == 1,
          "XX must overwrite an existing TTL");
    check(s.expire_in("k", 12000, ExpireFlags::Gt) == 1,
          "GT with a later deadline");
    check(s.expire_in("k", 8000, ExpireFlags::Gt) == 0,
          "GT with an earlier deadline");
    check(s.expire_in("k", 8000, ExpireFlags::Lt) == 1,
          "LT with an earlier deadline");
    check(s.expire_in("k", 20000, ExpireFlags::Lt) == 0,
          "LT with a later deadline");
    check(s.expire_in("k", 1000, ExpireFlags::Nx | ExpireFlags::Gt) == -1,
          "NX and GT are mutually exclusive");

    check(s.persist("k") == 1, "PERSIST removes the TTL");
    check(s.persist("k") == 0, "PERSIST on a key without TTL");
    check(s.pttl("k") == -1, "no TTL after PERSIST");

    check(s.expire_in("k", 5000) == 1, "EXPIRE again");
    check(s.set("k", "v1"), "plain SET must succeed");
    check(s.pttl("k") == -1, "plain SET must clear the TTL");

    check(s.expire_in("k", 5000) == 1, "EXPIRE for KEEPTTL test");
    check(s.set("k", "v2", /*keep_ttl=*/true), "SET KEEPTTL must succeed");
    check(s.pttl("k") == 5000, "SET KEEPTTL must preserve the TTL");

    now += 6000;
    check(s.del("k") == 0, "DEL of an already-expired key returns 0, like Redis");

    check(s.set("gone", "v"), "SET for the past-deadline test");
    check(s.expire_in("gone", -1) == 1, "EXPIRE in the past returns 1");
    check(!s.exists("gone"), "EXPIRE in the past deletes immediately");

    check(s.set("round", "v"), "SET for TTL rounding");
    check(s.expire_in("round", 1900) == 1, "EXPIRE 1900ms");
    check(s.ttl("round") == 2, "1900ms must round to 2 seconds");
    check(s.expire_in("round", 1400) == 1, "EXPIRE 1400ms");
    check(s.ttl("round") == 1, "1400ms must round to 1 second");
}

void test_lazy_get() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    for (int i = 0; i < 10; ++i) {
        check(s.set(key_of(i), value_of(32)), "SET for lazy test");
        check(s.expire_in(key_of(i), 1000) == 1, "EXPIRE for lazy test");
    }
    check(s.size() == 10, "10 keys before expiry");

    now += 1500;
    const u64 before = s.memory_used();
    check(!s.get("k0").has_value(), "an expired key must read as missing");
    check(s.size() == 9, "lazy expiration deletes the key");
    check(s.memory_used() < before, "lazy expiration frees memory");
    check(s.stats().lazy_expired == 1, "one lazy expiration");
    check(s.stats().expired_keys == 1, "expired_keys counts lazy deletions");

    check(!s.get("k0").has_value(), "the key stays missing");
    check(s.stats().lazy_expired == 1, "a second GET must not expire it again");
}

void test_lazy_write() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    check(s.set("k", "old"), "SET");
    check(s.expire_in("k", 1000) == 1, "EXPIRE");
    now += 2000;

    check(s.set("k", "new"), "overwriting an expired key");
    check(s.size() == 1, "overwrite must reuse the slot, not duplicate it");
    check(s.pttl("k") == -1, "a plain SET must not inherit the old TTL");
    check(s.get("k") == std::optional<std::string>("new"), "the new value must be visible");
}

}  // namespace archdesign::test
