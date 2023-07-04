#include <iostream>
#include <string>

#include <tao/pegtl.hpp>

#include "events.h"




struct AlgoCompiled {
    using PubkeySet = flat_hash_set<std::string>;
    std::vector<PubkeySet> pubkeySets;
    flat_hash_map<std::string, uint64_t> variableIndexLookup; // variableName -> index into pubkeySets
};


struct AlgoParseState {
    AlgoCompiled a;

    std::vector<AlgoCompiled::PubkeySet> pubkeySetStack;
    std::string currInfixOp;

    void letStart(std::string_view name) {
        a.variableIndexLookup[name] = a.pubkeySets.size();
        pubkeySetStack.push_back({});
    }

    void letEnd() {
        a.pubkeySets.emplace_back(std::move(pubkeySetStack.back()));
        pubkeySetStack.clear();
    }

    void letAddToPubkeySet(std::string_view id) {
        AlgoCompiled::PubkeySet set;

        if (id.starts_with('npub1')) {
        } else {
            if (!a.variableIndexLookup.contains(id)) throw herr("variable not found: ", id);
            auto n = a.variableIndexLookup[id];
            if (n >= a.pubkeySets.size()) throw herr("self referential variable: ", id);
        }
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




namespace pegtl = TAO_PEGTL_NAMESPACE;

namespace algo_parser {
    // Whitespace

    struct comment :
        pegtl::seq<
            pegtl::one< '#' >,
            pegtl::until< pegtl::eolf >
        > {};

    struct ws : pegtl::sor< pegtl::space, comment > {};

    template< typename R >
    struct pad : pegtl::pad< R, ws > {};


    // Pubkeys

    struct npub :
        pegtl::seq<
            pegtl::string< 'n', 'p', 'u', 'b', '1' >,
            pegtl::plus< pegtl::alnum >
        > {};

    struct pubkey :
        pegtl::sor<
            npub,
            pegtl::identifier
        > {};

    struct pubkeySetOp : pegtl::one< '+', '-' > {};

    struct pubkeyGroup;
    struct pubkeyList : pegtl::list< pubkeyGroup, pubkeySetOp, ws > {};

    struct pubkeyGroupOpen : pegtl::one< '(' > {};
    struct pubkeyGroupClose : pegtl::one< ')' > {};
    struct followerExpandHat : pegtl::one< '^' > {};

    struct pubkeyGroup : pegtl::sor<
        pegtl::seq<
            pubkey,
            pegtl::star<followerExpandHat>
        >,
        pegtl::seq<
            pad< pubkeyGroupOpen >,
            pubkeyList,
            pad< pubkeyGroupClose >
        >
    > {};



    // Let statements

    struct variableIdentifier : pegtl::seq< pegtl::not_at< npub >, pegtl::identifier > {};

    struct letIdentifier : variableIdentifier {};

    struct letTerminator : pegtl::one< ';' > {};

    struct let :
        pegtl::seq<
            pad< pegtl::string< 'l', 'e', 't' > >,
            pad< variableIdentifier >,
            pad< pegtl::one< '=' > >,
            pad< pubkeyList >,
            letTerminator
        > {};




    // Posts block

    struct number :
        pegtl::if_then_else< pegtl::one< '.' >,
            pegtl::plus< pegtl::digit >,
            pegtl::seq<
                pegtl::plus< pegtl::digit >,
                pegtl::opt< pegtl::one< '.' >, pegtl::star< pegtl::digit > >
            >
        > {};

    struct arith :
        pegtl::seq<
            pad< pegtl::one< '+', '-' > >,
            number
        > {};

    struct regexp :
        pegtl::seq<
            pegtl::one< '/' >,
            pegtl::star< pegtl::sor< pegtl::string< '\\', '/' >, pegtl::not_one< '/' > > >,
            pegtl::one< '/' >,
            pegtl::star< pegtl::alpha >
        > {};

    struct likedByCondition :
        pegtl::seq<
            pad< pegtl::string< 'l', 'i', 'k', 'e', 'd' > >,
            pad< pegtl::string< 'b', 'y' > >,
            pad< variableIdentifier >
        > {};

    struct contentCondition :
        pegtl::seq<
            pad< pegtl::string< 'c', 'o', 'n', 't', 'e', 'n', 't' > >,
            pad< pegtl::one< '~' > >,
            pad< regexp >
        > {};

    struct condition :
        pegtl::sor<
            pad< likedByCondition >,
            pad< contentCondition >
        > {};

    struct filterStatment :
        pegtl::seq<
            pegtl::sor<
                pad< pegtl::string< 'd', 'r', 'o', 'p' > >,
                pad< arith >
            >,
            pad< pegtl::string< 'i', 'f' > >,
            pad< condition >,
            pegtl::one< ';' >
        > {};

    struct postBlock :
        pegtl::seq<
            pad< pegtl::string< 'p', 'o', 's', 't' > >,
            pad< pegtl::one< '{' > >,
            pegtl::star< pad< filterStatment > >,
            pegtl::one< '}' >
        > {};




    // Main

    struct anything : pegtl::sor< ws, let, postBlock > {};
    struct main : pegtl::until< pegtl::eof, pegtl::must< anything > > {};



    template< typename Rule >
    struct action {};


    template<>
    struct action< letIdentifier > {
        template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            std::cout << "LET: " << in.string() << std::endl;
            a.letStart(in.string_view());
        }
    };

    template<>
    struct action< letIdentifier > {
        template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            std::cout << "LETEND: " << in.string() << std::endl;
            a.letEnd();
        }
    };

/*
    template<>
    struct action< pubkey > {
        template< typename ActionInput >
        static void apply(const ActionInput& in, AlgoParseState &state) {
            std::cout << "PK: " << in.string() << std::endl;
        }
    };

    template<>
    struct action< pubkeyGroupOpen > {
        template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &state) {
            std::cout << "PKGRPOPEN: " << in.string() << std::endl;
        }
    };

    template<>
    struct action< pubkeyGroupClose > {
        template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &state) {
            std::cout << "PKGRPCLOSE: " << in.string() << std::endl;
        }
    };
    */

/*
    template<>
    struct action< pubkeyList > {
        template< typename ActionInput >
        static void apply( const ActionInput& in, std::string& v ) {
            std::cout << "PKLIST: " << in.string() << std::endl;
        }
    };

    template<>
    struct action< pubkeySetOp > {
        template< typename ActionInput >
        static void apply( const ActionInput& in, std::string& v ) {
            std::cout << "PKOP: " << in.string() << std::endl;
        }
    };

    template<>
    struct action< regexp > {
        template< typename ActionInput >
        static void apply( const ActionInput& in, std::string& v ) {
            std::cout << "RE: " << in.string() << std::endl;
        }
    };
    */
}


inline AlgoParseState parseAlgo(lmdb::txn &txn, std::string_view algoText) {
    AlgoParseState state;

    pegtl::memory_input in(algoText, "");

    if (!pegtl::parse< algo_parser::main, algo_parser::action >(in, state)) {
        throw herr("algo parse error");
    }

    return state;
}
