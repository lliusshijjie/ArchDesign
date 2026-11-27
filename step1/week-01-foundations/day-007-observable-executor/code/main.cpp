#include <exception>
#include <iostream>
#include <string_view>

#include "scenarios.hpp"

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--self-test")) {
            archdesign::lab::run_scenario("all");
        } else if (argc == 3 && std::string_view(argv[1]) == "--scenario") {
            archdesign::lab::run_scenario(argv[2]);
        } else if (argc == 3 && std::string_view(argv[1]) == "--input") {
            archdesign::lab::run_input_file(argv[2]);
        } else if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "executor_lab [--self-test | --scenario NAME | --input PATH]\n"
                         "scenarios: normal ingress overload deadline slow-log failure-shutdown all\n";
        } else {
            std::cerr << "Invalid arguments. Use --help.\n";
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
