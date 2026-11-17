// SDRStudio - console self-test runner (no GUI, measurable in CI/terminal).
#include "../core/selftest.h"

int main() {
    return sdr::runSelfTest() ? 0 : 1;
}
