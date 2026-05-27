#include "config.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static app_config_t G;

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

static cJSON *default_config(void) {
    const char *s =
        "{"
        "\"listen_host\":\"0.0.0.0\","
        "\"listen_port\":8080,"
        "\"log_level\":\"info\","
        "\"realtime_print\":\"false\","
        "\"gateway_api_key\":\"cc-local-token\","
        "\"admin_token\":\"admin-local-token\","
        "\"admin_password\":\"topwalk\","
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
    /* Back-compat: add admin_password if missing */
    if (!cJSON_GetObjectItemCaseSensitive(root, "admin_password")) {
        cJSON_AddStringToObject(root, "admin_password", "topwalk");
        char *p = cJSON_Print(root);
        write_file_atomic(G.path, p);
        free(p);
        log_msg("INFO", "added default admin_password to config");
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

char *config_get_string_copy(const char *key) {
    pthread_rwlock_rdlock(&G.lock);
    const char *v = json_get_str(G.root, key);
    char *out = v ? xstrdup(v) : NULL;
    pthread_rwlock_unlock(&G.lock);
    return out;
}

char *config_masked_json(void) {
    pthread_rwlock_rdlock(&G.lock);
    char *out = cJSON_Print(G.root);
    pthread_rwlock_unlock(&G.lock);
    return out;
}

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

int config_set_string(const char *key, const char *value, char **err) {
    if (!key || !*key) { if (err) *err = xstrdup("missing key"); return -1; }
    pthread_rwlock_wrlock(&G.lock);
    cJSON *old = cJSON_GetObjectItemCaseSensitive(G.root, key);
    if (cJSON_IsString(old)) {
        cJSON_DeleteItemFromObject(G.root, key);
    }
    cJSON_AddStringToObject(G.root, key, value);
    char *txt = cJSON_Print(G.root);
    int rc = write_file_atomic(G.path, txt);
    free(txt);
    pthread_rwlock_unlock(&G.lock);
    if (rc != 0 && err) *err = xstrdup("failed to persist config");
    return rc;
}