#pragma once
// The bench (bench.cpp) and what each host's entry point provides for it.

// Runs the bench, prints the report and writes it to report_path if given;
// 0 when every checksum matched.
int bench_main(const char* kernels_exe, const char* lzo_path, const char* report_path);

const char* bench_host_name();
int bench_usable_cores();              // threads to try at most
void bench_thread_setup(int index);    // on each bench thread before it runs
void bench_progress(const char* line); // each report line, as it is printed
