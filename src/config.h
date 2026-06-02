/* ==========================================================================
 *  config.h — 网关配置管理模块的头文件
 *  -------------------------------------------------------------------------
 *  本模块负责 JSON 配置文件的加载、保存、热更新以及模型选择。
 *  所有配置数据通过读写锁保护，支持多线程并发读取和单线程独占写入。
 * ========================================================================== */

#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"
#include "log.h"

/**
 * config_load() — 加载网关配置文件
 * @param path:         JSON 配置文件路径（NULL 则使用 DEFAULT_CONFIG_PATH）
 * @param cli_port:     CLI -p 指定的端口，<=0 表示未指定
 * @param cli_password: CLI -P 指定的密码，NULL 表示未指定
 * @return: 成功返回 0；失败不返回（会创建默认配置）
 *
 * 工作原理：
 * 1. 读取指定路径的 JSON 文本并解析
 * 2. 解析失败时，生成内置默认配置（使用 cli_port/cli_password）并写回磁盘
 * 3. 解析成功后，提取 worker_threads、log_level、realtime_print 等关键字段
 *    更新全局状态（WORKER_COUNT、日志级别、实时打印模式）
 * 4. 若 cli_port/cli_password 指定，覆盖配置并持久化到磁盘
 */
int config_load(const char *path, long cli_port, const char *cli_password);

/**
 * config_masked_json() — 返回脱敏后的配置 JSON 字符串
 * @return: 动态分配的 JSON 字符串（调用者需 free），失败返回 NULL
 *
 * 工作原理：
 * 1. 复制当前完整配置树
 * 2. 遍历 models 数组，将所有 api_key 字段替换为 MASKED_KEY("***MASKED***")
 * 3. 返回格式化 JSON 文本，用于 Web 管理界面展示，避免密钥泄露
 */
char *config_masked_json(void);

/**
 * config_replace_from_json() — 热替换完整配置
 * @param body: 新的完整配置 JSON 文本
 * @param err:  输出参数，出错时写入错误信息（调用者需 free）
 * @return: 成功返回 0，失败返回 -1
 *
 * 工作原理：
 * 1. 解析 body 为 JSON，校验 models 是否为非空数组
 * 2. 写锁保护下，保留旧配置中对应模型的真实 api_key（当新配置中 api_key
 *    为 MASKED_KEY 时，说明前端未修改密钥，需沿用旧值）
 * 3. 原子写入文件（先写临时文件再 rename）
 * 4. 替换内存中的配置树，重新加载 worker_threads、log_level、realtime_print
 * 5. 释放旧配置树
 */
int config_replace_from_json(const char *body, char **err);

/**
 * config_select_model_copy() — 根据请求选择模型并返回独立副本
 * @param requested_model: 客户端请求的模型 ID（可为 NULL 或空字符串）
 * @return: 选中模型的深拷贝 cJSON 对象（调用者需 cJSON_Delete），未找到返回 NULL
 *
 * 模型选择优先级：
 * 1. 若指定了 requested_model 且该模型存在并启用 → 使用该模型
 * 2. 若指定模型不存在/被禁用 → 回退到 active_model（默认模型）
 * 3. 若未指定模型或 active_model 无效 → 回退到 models 数组中第一个启用的模型
 * 4. 返回的是副本，确保后续修改不会影响全局配置
 */
cJSON *config_select_model_copy(const char *requested_model);

/**
 * config_get_string_copy() — 获取配置中指定顶层字符串字段的值
 * @param key: 配置根对象中的键名
 * @return: 动态分配的字符串副本（调用者需 free），键不存在返回 NULL
 *
 * 读锁保护下的安全访问，常用于获取 gateway_api_key、admin_token 等字段
 */
char *config_get_string_copy(const char *key);

/**
 * config_set_active_model() — 设置并持久化默认活动模型
 * @param id:  模型 ID（必须非空）
 * @param err: 输出参数，出错时写入错误信息（调用者需 free）
 * @return: 成功返回 0，失败返回 -1
 *
 * 工作原理：
 * 1. 验证模型 ID 存在于当前配置中
 * 2. 写锁保护下修改 root->active_model 字段
 * 3. 原子写入配置文件，确保重启后仍生效
 */
int config_set_active_model(const char *id, char **err);
int config_set_string(const char *key, const char *value, char **err);

/* ---------- 模型级 cache_control 字段读取 ---------- */

/**
 * config_get_cache_policy() — 读取模型配置中的 cache_policy
 * @param model_cfg: 单个模型配置 cJSON 对象（config_select_model_copy 返回值或等价物）
 * @return: "off" / "auto" / 其他字符串。NULL 或缺省 → "off"
 */
const char *config_get_cache_policy(const cJSON *model_cfg);

/**
 * config_get_min_cache_tokens() — 读取模型配置中的 min_cache_tokens 阈值
 * @param model_cfg: 单个模型配置 cJSON 对象
 * @return: 缺省 1024
 */
int config_get_min_cache_tokens(const cJSON *model_cfg);

/**
 * config_get_prompt_tokens_includes_cache() — 读取模型配置中的 usage 统计口径
 * @param model_cfg:   单个模型配置 cJSON 对象
 * @param default_val: 字段未显式设置时的默认值
 * @return: true= prompt_tokens 已包含缓存 tokens
 *          false= prompt_tokens 与缓存分开统计
 *
 * 默认值选择：
 * - 非透传（OpenAI 格式）: true（DeepSeek/OpenAI 的 prompt_tokens 已含缓存）
 * - 透传（Anthropic 格式）: false（Anthropic 的 input_tokens 不含 cache_read_input_tokens）
 */
bool config_get_prompt_tokens_includes_cache(const cJSON *model_cfg, bool default_val);

#endif