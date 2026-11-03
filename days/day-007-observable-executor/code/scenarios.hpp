#pragma once

#include <string>
#include <string_view>

namespace archdesign::lab {

void run_scenario(std::string_view name);
void run_input_file(const std::string& path);

} // namespace archdesign::lab
