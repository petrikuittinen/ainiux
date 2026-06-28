#include <cstdlib>
#include <iostream>

#include "http/test_http.hpp"
#include "io/test_io_faults.hpp"
#include "support/test_support.hpp"

int main(int argc, char** argv) {
    const bool enospc_only = argc > 1 && argv[1] != nullptr && std::string(argv[1]) == "--enospc";

    if (enospc_only) {
        pkchat::test::io::run_enospc_all();
    } else {
        pkchat::test::io::run_readonly_all();
        pkchat::test::http::run_network_faults();
    }

    if (pkchat::test::failures != 0) {
        std::cerr << pkchat::test::failures << " io/network fault test(s) failed\n";
        return 1;
    }
    std::cout << (enospc_only ? "enospc mock tests passed\n" : "io/network fault tests passed\n");
    return 0;
}