// g++ -std=c++20 -g -I ../../../golpe/external/PEGTL/include/ tp.cpp 
// printf 'let asdf = npub1a + npub1b - npub1c;' | ./a.out

#include <iostream>
#include <string>

#include <tao/pegtl.hpp>

namespace pegtl = TAO_PEGTL_NAMESPACE;

namespace algo {
    // Whitespace

    struct comment :
        pegtl::seq<
            pegtl::one< '#' >,
            tao::pegtl::until< tao::pegtl::eolf >
        > {};

    struct ws : pegtl::sor< pegtl::space, comment > {};

    template< typename R >
    struct pad : tao::pegtl::pad< R, ws > {};


    // Pubkeys

    struct pubkey :
        pegtl::seq<
            pegtl::string< 'n', 'p', 'u', 'b', '1' >,
            pegtl::plus< pegtl::alnum >
        > {};

    struct pubkeySetOp : tao::pegtl::one< '+', '-' > {};

    struct pubkeyList : pegtl::list< pubkey, pubkeySetOp, ws > {};



    // Let statements

    struct letIdentifier : pegtl::identifier {};

    struct let :
        pegtl::seq<
            pad< pegtl::string< 'l', 'e', 't' > >,
            pad< letIdentifier >,
            pad< pegtl::one< '=' > >,
            pad< pubkeyList >,
            pegtl::one< ';' >
        > {};


    // Main

    struct anything : pegtl::sor< ws, let > {};
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
    if (pegtl::parse< algo::main, algo::action >( in, name )) {
        std::cout << name << std::endl;
    } else {
        std::cerr << "parse error" << std::endl;
    }
}
