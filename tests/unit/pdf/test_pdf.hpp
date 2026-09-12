#pragma once

namespace ainiux::test::pdf {

void run_all();
void set_runner_path(const char* path);
void run_adversarial();
int run_adversarial_case(const char* name);

}  // namespace ainiux::test::pdf
