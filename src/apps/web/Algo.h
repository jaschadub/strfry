#pragma once

#include "golpe.h"

#include "events.h"


struct Algo {
    flat_hash_set<std::string> following;


    Algo(lmdb::txn &txn, std::string_view pubkey) {
        following = loadFollowing(txn, pubkey);
    }


    std::vector<uint64_t> getEvents(lmdb::txn &txn, uint64_t limit) {
        std::vector<uint64_t> output;

        env.generic_foreachFull(txn, env.dbi_Event__created_at, lmdb::to_sv<uint64_t>(MAX_U64), lmdb::to_sv<uint64_t>(MAX_U64), [&](auto k, auto v) {
            if (output.size() > limit) return false;

            auto ev = lookupEventByLevId(txn, lmdb::from_sv<uint64_t>(v));

            if (ev.flat_nested()->kind() != 1) return true;
            if (!following.contains(sv(ev.flat_nested()->pubkey()))) return true;

            for (const auto &tagPair : *(ev.flat_nested()->tagsFixed32())) {
                if ((char)tagPair->key() != 'e') return true;
            }

            output.emplace_back(ev.primaryKeyId);

            return true;
        }, true);

        return output;
    }


    flat_hash_set<std::string> loadFollowing(lmdb::txn &txn, std::string_view pubkey) {
        flat_hash_set<std::string> output;

        const uint64_t kind = 3;

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, kind, 0), "", [&](std::string_view k, std
::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == kind) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                auto ev = lookupEventByLevId(txn, levId);

                for (const auto &tagPair : *(ev.flat_nested()->tagsFixed32())) {
                    if ((char)tagPair->key() != 'p') continue;
                    output.insert(std::string(sv(tagPair->val())));
                }
            }

            return false;
        });

        return output;
    }
};
