// g++ -std=c++20 -g -I ../../../golpe/external/PEGTL/include/ tp.cpp 
// printf 'let asdf = npub1a + npub1b - npub1c;' | ./a.out

#include <iostream>
#include <string>

#include <tao/pegtl.hpp>

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

    struct pubkeyGroup : pegtl::sor<
        pubkey,
        pegtl::seq<
            pad< pubkeyGroupOpen >,
            pubkeyList,
            pad< pubkeyGroupClose >
        >
    > {};



    // Let statements

    struct letIdentifier : pegtl::identifier {};

    struct variableIdentifier : pegtl::seq< pegtl::not_at< npub >, pegtl::identifier > {};

    struct let :
        pegtl::seq<
            pad< pegtl::string< 'l', 'e', 't' > >,
            pad< variableIdentifier >,
            pad< pegtl::one< '=' > >,
            pad< pubkeyList >,
            pegtl::one< ';' >
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
        static void apply( const ActionInput& in, std::string& v ) {
            std::cout << "LET: " << in.string() << std::endl;
        }
    };

    template<>
    struct action< pubkey > {
        template< typename ActionInput >
        static void apply( const ActionInput& in, std::string& v ) {
            std::cout << "PK: " << in.string() << std::endl;
        }
    };

    template<>
    struct action< pubkeyGroupOpen > {
        template< typename ActionInput >
        static void apply( const ActionInput& in, std::string& v ) {
            std::cout << "PKGRPOPEN: " << in.string() << std::endl;
        }
    };

    template<>
    struct action< pubkeyGroupClose > {
        template< typename ActionInput >
        static void apply( const ActionInput& in, std::string& v ) {
            std::cout << "PKGRPCLOSE: " << in.string() << std::endl;
        }
    };

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
}

int main( int argc, char** argv ) {
    std::string str;

    {
        std::string line;
        while (std::getline(std::cin, line)) {
            str += line;
            str += "\n";
        }
    }

    std::string name;

    pegtl::memory_input in(str, "std::cin");
    if (pegtl::parse< algo_parser::main, algo_parser::action >( in, name )) {
        std::cout << name << std::endl;
    } else {
        std::cerr << "parse error" << std::endl;
    }
}
