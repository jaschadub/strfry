#pragma once

#include "golpe.h"

#include "events.h"


struct Algo {
    struct EventInfo {
        uint64_t comments = 0;
        double score = 0.0;
    };

    struct FilteredEvent {
        uint64_t levId;
        std::string id;

        EventInfo info;
    };

    flat_hash_set<std::string> following;


    Algo(lmdb::txn &txn, std::string_view pubkey) {
        flat_hash_set<std::string> f1;

        loadFollowing(txn, pubkey, f1);
        LI << "1FOL = " << f1.size();

        auto cp = f1;
        for (auto &p : cp) loadFollowing(txn, p, f1);
        LI << "2FOL = " << f1.size();

        f1.insert(std::string(pubkey));

        std::swap(following, f1);
    }


    std::vector<FilteredEvent> getEvents(lmdb::txn &txn, uint64_t limit) {
        flat_hash_map<std::string, EventInfo> eventInfoCache;
        std::vector<FilteredEvent> output;

        env.generic_foreachFull(txn, env.dbi_Event__created_at, lmdb::to_sv<uint64_t>(MAX_U64), lmdb::to_sv<uint64_t>(MAX_U64), [&](auto k, auto v) {
            if (output.size() > limit) return false;

            auto ev = lookupEventByLevId(txn, lmdb::from_sv<uint64_t>(v));
            auto kind = ev.flat_nested()->kind();
            auto id = sv(ev.flat_nested()->id());

if (id == from_hex("24b371a0ef8cd3a072b66d802dab3bd9a1c4a03b57b99fed388a7573cbc60852")) LI << "BING";
            if (kind == 1) {
                auto pubkey = std::string(sv(ev.flat_nested()->pubkey()));

                bool foundETag = false;
                for (const auto &tagPair : *(ev.flat_nested()->tagsFixed32())) {
                    if ((char)tagPair->key() == 'e') {
                        auto tagEventId = std::string(sv(tagPair->val()));
if (id == from_hex("24b371a0ef8cd3a072b66d802dab3bd9a1c4a03b57b99fed388a7573cbc60852")) LI << "Z: " << to_hex(tagEventId);
                        eventInfoCache.emplace(tagEventId, EventInfo{});
                        eventInfoCache[tagEventId].comments++;
                        foundETag = true;
                    }
                }
                if (foundETag) return true; // not root event

                eventInfoCache.emplace(id, EventInfo{});
                auto &eventInfo = eventInfoCache[id];

                if (!following.contains(pubkey)) return true;
                if (eventInfo.score < 5.0) return true;

                output.emplace_back(FilteredEvent{ev.primaryKeyId, std::string(id), eventInfo});
            } else if (kind == 7) {
                auto pubkey = std::string(sv(ev.flat_nested()->pubkey()));
                //if (!following.contains(pubkey)) return true;

                const auto &tagsArr = *(ev.flat_nested()->tagsFixed32());
                for (auto it = tagsArr.rbegin(); it != tagsArr.rend(); ++it) {
                    auto tagPair = *it;
                    if ((char)tagPair->key() == 'e') {
                        auto tagEventId = std::string(sv(tagPair->val()));
                        eventInfoCache.emplace(tagEventId, EventInfo{});
                        eventInfoCache[tagEventId].score++;
                        break;
                    }
                }
            }

            return true;
        }, true);

        //for (auto &o : output) {
        //}

        return output;
    }


    void loadFollowing(lmdb::txn &txn, std::string_view pubkey, flat_hash_set<std::string> &output) {
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
    }
};
