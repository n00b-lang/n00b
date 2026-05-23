/* WP-005 fixture: minimal hello program compiled at configure time
 * for use as a real-binary test fixture (ELF on Linux, Mach-O on
 * macOS, PE on Windows). Used by test_attest_extract_from_artifact
 * (and future P3/P6 tests) to exercise libchalk's codec path on
 * production-realistic binaries rather than synthetic in-process
 * builds from n00b_macho_build() / n00b_elf_build().
 */
int main(void) { return 0; }
