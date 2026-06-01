/* File: tests/test_stubs.c
 * 提供 config.c 引用、main.c 定义的符号 stub。
 * 仅为需要 config.c 的 unit test 链接（test_cache_control）。
 * 用 weak 符号避免与已有 test 源文件里的同名定义冲突。 */
__attribute__((weak)) int WORKER_COUNT = 4;
