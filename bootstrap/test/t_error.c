// Test: #error directive causes ncc to exit with non-zero status.
// This is critical for CMake/Meson feature detection.
#error "deliberate error for testing"
