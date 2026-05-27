/* ==========================================================================
 *  config.c — 网关配置管理模块的实现
 *  -------------------------------------------------------------------------
 *  提供配置文件的加载、保存、热更新及模型选择功能。
 *  核心数据结构：
 *    - G (app_config_t): 全局配置单例，受读写锁保护
 *    - G.root (cJSON*): 当前加载的完整配置树
 *    - G.path: 配置文件的磁盘路径
 *  线程安全：所有对 G.root 的访问均通过 pthread_rwlock_t 同步
 * ========================================================================== */

#include "config.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/**
 * G — 全局配置实例（静态单例）
 * 包含：
 *   lock: 读写锁，保证多线程下配置访问安全
 *   root: cJSON 配置树根节点
 *   path: 配置文件在磁盘上的绝对或相对路径
 */
static app_config_t G;

/**
 * read_file() — 读取文件全部内容为 C 字符串
 * @param path:    文件路径
 * @param len_out: 输出参数，返回实际读取字节数（可为 NULL）
 * @return: 动态分配的以 '\0' 结尾的缓冲区（调用者需 free），失败返回 NULL
 *
 * 实现细节：
 * 1. 以二进制模式打开文件，避免平台换行符转换差异
 * 2. SEEK_END + ftell 获取文件大小
 * 3. calloc 分配 sz+1 字节，确保末尾自动为 '\0'
 * 4. 即使 fread 读取字节数小于 sz（罕见），缓冲区依然安全终止
 */
static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)calloc(1, (size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    if (len_out) *len_out = n;
    return buf;
}

/**
 * write_file_atomic() — 原子方式写入文件
 * @param path: 目标文件路径
 * @param data: 待写入的字符串内容
 * @return: 成功返回 0，失败返回 -1
 *
 * 原子性保证：
 * 1. 生成临时文件名：原路径 + ".tmp." + PID，避免多进程冲突
 * 2. 先完整写入临时文件，fflush + fsync 确保数据落盘（崩溃安全）
 * 3. 最后 rename(2) 将临时文件覆盖为目标文件
 *    rename 在 POSIX 上是原子操作，不会出现半写状态
 * 4. 任何中间失败都会删除临时文件，避免残留垃圾
 */
static int write_file_atomic(const char *path, const char *data) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t len = strlen(data);
    if (fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp, path);
}

/**
 * default_config() — 生成内置默认配置
 * @return: 默认配置的 cJSON 对象（调用者需管理生命周期）
 *
 * 当配置文件缺失或 JSON 解析失败时调用，确保网关能立即启动。
 * 默认配置包含：
 *   - 监听地址 0.0.0.0:8080
 *   - 日志级别 info，关闭实时打印
 *   - gateway_api_key / admin_token 使用本地开发默认值
 *   - 4 个 worker 线程
 *   - 预置一个 models 条目（阿里云百炼 qwen-coder）
 * 生成后会自动写回磁盘，方便用户在此基础上修改
 */
static cJSON *default_config(void) {
    const char *s =
        "{"
        "\"listen_host\":\"0.0.0.0\","
        "\"listen_port\":8080,"
        "\"log_level\":\"info\","
        "\"realtime_print\":\"false\","
        "\"gateway_api_key\":\"cc-local-token\","
        "\"admin_token\":\"admin-local-token\","
        "\"worker_threads\":4,"
        "\"active_model\":\"qwen-coder\","
        "\"models\":[{"
          "\"id\":\"qwen-coder\","
          "\"provider\":\"aliyun-bailian\","
          "\"display_name\":\"Qwen Coder via OpenAI Compatible\","
          "\"interface\":\"openai_chat_completions\","
          "\"base_url\":\"https://dashscope.aliyuncs.com/compatible-mode/v1\","
          "\"endpoint\":\"\","
          "\"api_key\":\"YOUR_OPENAI_COMPATIBLE_API_KEY\","
          "\"upstream_model\":\"qwen3-coder-plus\","
          "\"enabled\":true,"
          "\"params\":{\"temperature\":0.2,\"top_p\":0.95},"
          "\"extra_body\":{}"
        "}]"
        "}";
    return cJSON_Parse(s);
}

/**
 * find_model_by_id() — 在配置中按模型 ID 查找模型对象
 * @param root: 配置树根节点
 * @param id:   要查找的模型 ID
 * @return: 找到的模型 cJSON 对象指针，未找到返回 NULL
 *
 * 遍历 root->models 数组，进行大小写敏感的字符串比较
 */
static cJSON *find_model_by_id(cJSON *root, const char *id) {
    cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (!cJSON_IsArray(models) || !id) return NULL;
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *mid = json_get_str(m, "id");
        if (mid && strcmp(mid, id) == 0) return m;
    }
    return NULL;
}

/**
 * preserve_masked_keys() — 热更新时保留未修改的 API 密钥
 * @param incoming: 新的配置树（将被修改）
 * @param oldroot:  旧的配置树（只读，用于提取真实密钥）
 *
 * 背景：Web 管理界面展示配置时会把 api_key 脱敏为 MASKED_KEY。
 * 当管理员只修改其他字段（如 temperature）而未触碰密钥时，
 * 前端回传的 JSON 中 api_key 仍然是 MASKED_KEY。
 * 本函数遍历新配置中的每个模型：
 *   若 api_key == MASKED_KEY，则在旧配置中查找同名模型，
 *   将其真实 api_key 复制到新配置中，避免密钥被覆盖为空或掩码值。
 */
static void preserve_masked_keys(cJSON *incoming, cJSON *oldroot) {
    cJSON *models = cJSON_GetObjectItemCaseSensitive(incoming, "models");
    if (!cJSON_IsArray(models) || !oldroot) return;
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *id = json_get_str(m, "id");
        cJSON *k = cJSON_GetObjectItemCaseSensitive(m, "api_key");
        if (!id || !cJSON_IsString(k) || strcmp(k->valuestring, MASKED_KEY) != 0) continue;
        cJSON *oldm = find_model_by_id(oldroot, id);
        const char *oldk = oldm ? json_get_str(oldm, "api_key") : NULL;
        if (oldk) {
            cJSON_DeleteItemFromObject(m, "api_key");
            cJSON_AddStringToObject(m, "api_key", oldk);
        }
    }
}

/**
 * config_load() — 加载并初始化网关配置（参见 config.h 声明）
 *
 * 详细流程：
 * 1. 初始化读写锁 G.lock
 * 2. 确定配置文件路径（参数 path 优先，否则 DEFAULT_CONFIG_PATH）
 * 3. 读取文件内容并解析 JSON：
 *    - 成功 → 使用解析后的配置树
 *    - 失败 → 调用 default_config() 生成默认配置，并原子写入磁盘
 * 4. 提取并校验 worker_threads：
 *    - 范围限制 [1, MAX_WORKERS]，防止非法值导致线程异常
 * 5. 设置日志级别：
 *    - 从配置读取 "log_level" 字符串，传给 log_set_level()
 * 6. 设置实时打印模式：
 *    - "all"  → RT_ALL（打印所有流式数据）
 *    - "txt"/"text" → RT_TXT（仅打印文本内容）
 *    - 其他   → RT_OFF（关闭）
 * 7. 存储根配置到 G.root，供后续查询
 */
int config_load(const char *path) {
    pthread_rwlock_init(&G.lock, NULL);
    snprintf(G.path, sizeof(G.path), "%s", path ? path : DEFAULT_CONFIG_PATH);
    size_t n = 0;
    char *txt = read_file(G.path, &n);
    cJSON *root = NULL;
    if (txt) root = cJSON_Parse(txt);
    free(txt);
    if (!root) {
        root = default_config();
        char *p = cJSON_Print(root);
        write_file_atomic(G.path, p);
        free(p);
        log_msg("WARN", "created default config: %s", G.path);
    }
    G.root = root;
    WORKER_COUNT = (int)json_get_long(root, "worker_threads", 4);
    if (WORKER_COUNT < 1) WORKER_COUNT = 1;
    if (WORKER_COUNT > MAX_WORKERS) WORKER_COUNT = MAX_WORKERS;
    log_set_level(json_get_str(root, "log_level"));
    {   const char *rm = json_get_str(root, "realtime_print");
        if (rm && strcmp(rm, "all") == 0) rt_set_mode(RT_ALL);
        else if (rm && (strcmp(rm, "txt") == 0 || strcmp(rm, "text") == 0)) rt_set_mode(RT_TXT);
        else rt_set_mode(RT_OFF);
    }
    return 0;
}

/**
 * config_select_model_copy() — 根据请求选择模型并返回独立副本（参见 config.h 声明）
 *
 * 线程安全：使用读锁保护对 G.root 的访问。
 * 回退策略（按优先级）：
 *   1. 请求的模型存在且启用
 *   2. active_model（默认模型）存在且启用
 *   3. models 数组中第一个启用的模型
 * 返回深拷贝的原因：
 *   调用者（如 worker 线程）需要长期持有模型配置进行上游请求，
 *   深拷贝避免在热更新时旧配置被释放导致悬空指针。
 */
cJSON *config_select_model_copy(const char *requested_model) {
    pthread_rwlock_rdlock(&G.lock);
    cJSON *m = NULL;
    if (requested_model && *requested_model) {
        m = find_model_by_id(G.root, requested_model);
        if (!m || !json_get_bool(m, "enabled", true)) {
            log_msg("WARN", "model '%s' not found or disabled, checking active_model", requested_model);
            const char *active = json_get_str(G.root, "active_model");
            if (active) m = find_model_by_id(G.root, active);
            else m = NULL;
        }
    } else {
        const char *active = json_get_str(G.root, "active_model");
        if (active) m = find_model_by_id(G.root, active);
    }
    if (!m || !json_get_bool(m, "enabled", true)) {
        cJSON *models = cJSON_GetObjectItemCaseSensitive(G.root, "models");
        cJSON *it;
        cJSON_ArrayForEach(it, models) {
            if (json_get_bool(it, "enabled", true)) { m = it; break; }
        }
    }
    cJSON *copy = m ? cJSON_Duplicate(m, 1) : NULL;
    pthread_rwlock_unlock(&G.lock);
    return copy;
}

/**
 * config_get_string_copy() — 获取配置中指定顶层字符串字段的值（参见 config.h 声明）
 *
 * 读锁保护下访问 G.root，提取 key 对应的字符串值，
 * 使用 xstrdup（内部实现通常为 strdup 的包装）创建副本后返回。
 * 调用者负责 free 返回的字符串。
 */
char *config_get_string_copy(const char *key) {
    pthread_rwlock_rdlock(&G.lock);
    const char *v = json_get_str(G.root, key);
    char *out = v ? xstrdup(v) : NULL;
    pthread_rwlock_unlock(&G.lock);
    return out;
}

/**
 * config_masked_json() — 返回脱敏后的配置 JSON 字符串（参见 config.h 声明）
 *
 * 实现流程：
 * 1. 读锁下深拷贝整个配置树，解锁后立即释放锁，减少临界区
 * 2. 遍历 models 数组，对每个 api_key 字段：
 *    - 若存在且非空，删除原值，替换为 MASKED_KEY("***MASKED***")
 * 3. 格式化输出 JSON 字符串，释放拷贝树
 * 4. 返回格式化字符串（调用者 free）
 */
char *config_masked_json(void) {
    pthread_rwlock_rdlock(&G.lock);
    cJSON *copy = cJSON_Duplicate(G.root, 1);
    pthread_rwlock_unlock(&G.lock);
    cJSON *models = cJSON_GetObjectItemCaseSensitive(copy, "models");
    if (cJSON_IsArray(models)) {
        cJSON *m;
        cJSON_ArrayForEach(m, models) {
            cJSON *k = cJSON_GetObjectItemCaseSensitive(m, "api_key");
            if (cJSON_IsString(k) && k->valuestring && strlen(k->valuestring) > 0) {
                cJSON_DeleteItemFromObject(m, "api_key");
                cJSON_AddStringToObject(m, "api_key", MASKED_KEY);
            }
        }
    }
    char *out = cJSON_Print(copy);
    cJSON_Delete(copy);
    return out;
}

/**
 * config_replace_from_json() — 热替换完整配置（参见 config.h 声明）
 *
 * 实现流程：
 * 1. 解析 body 为 JSON，基础校验（models 必须为非空数组）
 * 2. 获取写锁，独占访问 G.root
 * 3. 调用 preserve_masked_keys()：若新配置中某模型 api_key 为 MASKED_KEY，
 *    则从旧配置中保留真实密钥，防止前端未修改时密钥丢失
 * 4. 将新配置格式化并原子写入磁盘（write_file_atomic）
 *    - 写入失败 → 释放锁、清理资源、返回错误
 * 5. 写入成功后：
 *    - 删除旧配置树 cJSON_Delete(G.root)
 *    - G.root = newroot
 *    - 重新加载 worker_threads（限制范围 [1, MAX_WORKERS]）
 *    - 重新加载 log_level、realtime_print
 * 6. 释放写锁，返回成功
 *
 * 注意：步骤 4 和 5 均在锁内完成，保证内存状态与磁盘状态始终一致
 */
int config_replace_from_json(const char *body, char **err) {
    cJSON *newroot = cJSON_Parse(body);
    if (!newroot) {
        if (err) *err = xstrdup("invalid JSON");
        return -1;
    }
    cJSON *models = cJSON_GetObjectItemCaseSensitive(newroot, "models");
    if (!cJSON_IsArray(models) || cJSON_GetArraySize(models) == 0) {
        cJSON_Delete(newroot);
        if (err) *err = xstrdup("models must be a non-empty array");
        return -1;
    }
    pthread_rwlock_wrlock(&G.lock);
    preserve_masked_keys(newroot, G.root);
    char *txt = cJSON_Print(newroot);
    if (write_file_atomic(G.path, txt) != 0) {
        pthread_rwlock_unlock(&G.lock);
        cJSON_Delete(newroot);
        free(txt);
        if (err) *err = xstrdup("failed to persist config");
        return -1;
    }
    cJSON_Delete(G.root);
    G.root = newroot;
    WORKER_COUNT = (int)json_get_long(G.root, "worker_threads", WORKER_COUNT);
    if (WORKER_COUNT < 1) WORKER_COUNT = 1;
    if (WORKER_COUNT > MAX_WORKERS) WORKER_COUNT = MAX_WORKERS;
    log_set_level(json_get_str(G.root, "log_level"));
    {   const char *rm = json_get_str(G.root, "realtime_print");
        if (rm && strcmp(rm, "all") == 0) rt_set_mode(RT_ALL);
        else if (rm && (strcmp(rm, "txt") == 0 || strcmp(rm, "text") == 0)) rt_set_mode(RT_TXT);
        else rt_set_mode(RT_OFF);
    }
    pthread_rwlock_unlock(&G.lock);
    free(txt);
    return 0;
}

/**
 * config_set_active_model() — 设置并持久化默认活动模型（参见 config.h 声明）
 *
 * 实现流程：
 * 1. 参数校验：id 必须非空
 * 2. 获取写锁
 * 3. 校验模型 ID 存在于当前配置的 models 数组中
 *    - 不存在 → 解锁，返回错误
 * 4. 修改配置树：
 *    - 若已存在 active_model 字段，先删除
 *    - 添加新的 active_model 字符串
 * 5. 格式化整棵配置树，原子写入磁盘
 * 6. 解锁，根据写入结果返回
 */
int config_set_active_model(const char *id, char **err) {
    if (!id || !*id) { if (err) *err = xstrdup("missing active_model"); return -1; }
    pthread_rwlock_wrlock(&G.lock);
    cJSON *m = find_model_by_id(G.root, id);
    if (!m) {
        pthread_rwlock_unlock(&G.lock);
        if (err) *err = xstrdup("model id not found");
        return -1;
    }
    cJSON *active = cJSON_GetObjectItemCaseSensitive(G.root, "active_model");
    if (cJSON_IsString(active)) {
        cJSON_DeleteItemFromObject(G.root, "active_model");
    }
    cJSON_AddStringToObject(G.root, "active_model", id);
    char *txt = cJSON_Print(G.root);
    int rc = write_file_atomic(G.path, txt);
    free(txt);
    pthread_rwlock_unlock(&G.lock);
    if (rc != 0 && err) *err = xstrdup("failed to persist active_model");
    return rc;
}