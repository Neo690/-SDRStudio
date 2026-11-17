// SDRStudio - self-test / benchmark entry point.
#pragma once

namespace sdr {
// Runs demodulation accuracy tests and a throughput benchmark.
// Returns true when every check passed. Prints a report to stdout.
bool runSelfTest();
} // namespace sdr
