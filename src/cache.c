

#include "cache.h"
#include "web.h"
#include "fs_port.h"
#include "fs_ext.h"
#include "os_port.h"
#include "server_helpers.h"
#include "hash/sha256.h" // for sha256Update, sha256Final, sha256Init

#define CACHE_INDEX_FILENAME "cache_index.txt"
#define CACHE_INDEX_SEP '\t'

static void cache_index_append(const char *cached_url, const char *original_url);

cache_entry_t cache_table = {.next = NULL, .hash = 0, .original_url = NULL, .cached_url = NULL, .file_path = NULL};
uint32_t cache_entries = 0;

uint32_t cache_flush()
{
    uint32_t deleted = 0;
    cache_entry_t *pos = &cache_table;

    while (pos != NULL)
    {
        if (pos->exists)
        {
            // Attempt to delete the local file
            if (pos->file_path && fsDeleteFile(pos->file_path) == NO_ERROR)
            {
                deleted++;
                TRACE_INFO("Deleted cached file: %s\n", pos->file_path);
            }
            else
            {
                TRACE_WARNING("Failed to delete cached file: %s\n", pos->file_path ? pos->file_path : "Unknown path");
            }

            // Set the exists flag to false
            pos->exists = false;
        }

        pos = pos->next;
    }

    return deleted;
}

/**
 * @brief Gathers statistics about the current cache.
 *
 * @param stats Pointer to a structure where the statistics will be stored.
 */
void cache_stats(cache_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }

    cache_entry_t *pos = &cache_table;
    memset(stats, 0, sizeof(cache_stats_t)); // Initialize all stats to zero

    while (pos != NULL)
    {
        stats->total_entries++;
        stats->memory_used += sizeof(*pos); // Add size of the cache entry structure

        if (pos->original_url)
        {
            stats->memory_used += strlen(pos->original_url) + 1; // Add length of original_url string
        }
        if (pos->cached_url)
        {
            stats->memory_used += strlen(pos->cached_url) + 1; // Add length of cached_url string
        }
        if (pos->file_path)
        {
            stats->memory_used += strlen(pos->file_path) + 1; // Add length of file_path string
        }
        if (pos->exists)
        {
            stats->exists_entries++;
        }
        if (fsFileExists(pos->file_path))
        {
            stats->total_files++;
            uint32_t size = 0;
            fsGetFileSize(pos->file_path, &size);
            stats->total_size += size;
        }
        pos = pos->next;
    }
}

void cache_entry_add(cache_entry_t *entry)
{
    if (!entry)
    {
        TRACE_ERROR("entry is NULL\r\n");
        return;
    }

    cache_entry_t *pos = &cache_table;

    if (!pos)
    {
        TRACE_ERROR("cache_table is NULL\r\n");
        return;
    }

    TRACE_DEBUG("Starting to add cache entry with the following details:\r\n");
    TRACE_DEBUG("  Hash: %08X\r\n", entry->hash);
    TRACE_DEBUG("  Original URL: %s\r\n", entry->original_url ? entry->original_url : "NULL");
    TRACE_DEBUG("  Cached URL: %s\r\n", entry->cached_url ? entry->cached_url : "NULL");
    TRACE_DEBUG("  File Path: %s\r\n", entry->file_path ? entry->file_path : "NULL");

    while (pos)
    {
        cache_entry_t *next = pos->next;

        if (!next)
        {
            TRACE_DEBUG("End of list reached, adding entry with hash: %08X at the end\r\n", entry->hash);
            pos->next = entry;
            entry->next = NULL;
            return;
        }

        if (entry->hash < next->hash)
        {
            TRACE_DEBUG("Inserting entry with hash: %08X before entry with hash: %08X\r\n", entry->hash, next->hash);
            pos->next = entry;
            entry->next = next;
            return;
        }

        if (entry->hash == next->hash)
        {
            if (!osStrcmp(entry->original_url, next->original_url))
            {
                TRACE_DEBUG("Already added: %08X\r\n", entry->hash);
                return;
            }
            TRACE_DEBUG("Inserting (duplicate short hash) entry with hash: %08X before entry with hash: %08X\r\n", entry->hash, next->hash);
            pos->next = entry;
            entry->next = next;
            return;
        }

        pos = next;
    }

    TRACE_DEBUG("Finished adding cache entry with hash: %08X\r\n", entry->hash);
}

cache_entry_t *cache_add(const char *url)
{
    const char *cachePath = get_settings()->internal.cachedirfull;

    if (cachePath == NULL || !fsDirExists(cachePath))
    {
        TRACE_ERROR("core.cachedirfull not set to a valid path: '%s'", cachePath);
        return NULL;
    }

    uint8_t sha256_calc[SHA256_DIGEST_SIZE];
    char sha256_calc_str[2 * SHA256_DIGEST_SIZE + 1];

    /* hash the image URL */
    Sha256Context ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, url, strlen(url));
    sha256Final(&ctx, sha256_calc);

    for (int pos = 0; pos < SHA256_DIGEST_SIZE; pos++)
    {
        osSprintf(&sha256_calc_str[2 * pos], "%02X", sha256_calc[pos]);
    }

    /* Find the file extension from the URL */
    const char *ext_pos = strrchr(url, '.');
    char *extension = strdup("jpg");

    if (ext_pos && !osStrchr(ext_pos, '/'))
    {
        osFreeMem(extension);
        extension = strdup(&ext_pos[1]);

        /* Remove optional HTTP GET parameters */
        char *query_param = osStrchr(extension, '?');
        if (query_param)
        {
            *query_param = '\0';
        }
    }

    cache_entry_t *entry = osAllocMem(sizeof(cache_entry_t));

    entry->hash = ((uint32_t)sha256_calc[0] << 24) | ((uint32_t)sha256_calc[1] << 16) | ((uint32_t)sha256_calc[2] << 8) | (sha256_calc[3] << 0);
    entry->original_url = strdup(url);
    entry->file_path = custom_asprintf("%s%c%s.%s", cachePath, PATH_SEPARATOR, sha256_calc_str, extension);
    entry->cached_url = custom_asprintf("/cache/%s.%s", sha256_calc_str, extension);
    entry->exists = fsFileExists(entry->file_path);

    cache_entry_add(entry);

    cache_index_append(entry->cached_url, entry->original_url);

    osFreeMem(extension);

    return entry;
}

static void cache_index_append(const char *cached_url, const char *original_url)
{
    const char *cachePath = get_settings()->internal.cachedirfull;
    if (!cachePath || !cached_url || !original_url)
        return;

    char *indexPath = custom_asprintf("%s%c%s", cachePath, PATH_SEPARATOR, CACHE_INDEX_FILENAME);
    if (!indexPath)
        return;

    FsFile *f = fsOpenFileEx(indexPath, "a");
    osFreeMem(indexPath);
    if (!f)
        return;

    size_t lineLen = osStrlen(cached_url) + 1 + osStrlen(original_url) + 1;
    char *line = osAllocMem(lineLen + 1);
    if (!line)
    {
        fsCloseFile(f);
        return;
    }
    osSnprintf(line, lineLen + 1, "%s%c%s\n", cached_url, CACHE_INDEX_SEP, original_url);
    fsWriteFile(f, line, osStrlen(line));
    osFreeMem(line);
    fsCloseFile(f);
}

bool cache_index_lookup(const char *cached_url, char **original_url_out)
{
    if (!cached_url || !original_url_out)
        return false;
    *original_url_out = NULL;

    const char *cachePath = get_settings()->internal.cachedirfull;
    if (!cachePath || !fsDirExists(cachePath))
        return false;

    char *indexPath = custom_asprintf("%s%c%s", cachePath, PATH_SEPARATOR, CACHE_INDEX_FILENAME);
    if (!indexPath)
        return false;

    if (!fsFileExists(indexPath))
    {
        osFreeMem(indexPath);
        return false;
    }

    uint32_t fileSize32 = 0;
    if (fsGetFileSize(indexPath, &fileSize32) != NO_ERROR)
    {
        osFreeMem(indexPath);
        return false;
    }
    size_t fileSize = fileSize32;

    FsFile *f = fsOpenFile(indexPath, FS_FILE_MODE_READ);
    osFreeMem(indexPath);
    if (!f)
        return false;
    if (fileSize == 0 || fileSize > 1024 * 1024)
    {
        fsCloseFile(f);
        return false;
    }

    char *data = osAllocMem(fileSize + 1);
    if (!data)
    {
        fsCloseFile(f);
        return false;
    }

    size_t pos = 0;
    size_t sizeRead = 0;
    while (pos < fileSize && fsReadFile(f, &data[pos], fileSize - pos, &sizeRead) == NO_ERROR && sizeRead > 0)
    {
        pos += sizeRead;
    }
    fsCloseFile(f);
    data[pos] = '\0';

    char *found = NULL;
    char *line = data;
    while (line && *line)
    {
        char *sep = osStrchr(line, CACHE_INDEX_SEP);
        char *eol = osStrchr(line, '\n');
        if (!sep || (eol && sep > eol))
        {
            line = eol ? eol + 1 : NULL;
            continue;
        }
        *sep = '\0';
        if (osStrcmp(line, cached_url) == 0)
        {
            char *orig = sep + 1;
            if (eol)
                *eol = '\0';
            osFreeMem(found);
            found = strdup(orig);
        }
        line = eol ? eol + 1 : NULL;
    }
    osFreeMem(data);
    if (found)
    {
        *original_url_out = found;
        return true;
    }
    return false;
}

cache_entry_t *cache_create_redirect_entry(const char *cached_url, const char *original_url)
{
    if (!cached_url || !original_url)
        return NULL;

    const char *cache_pos = osStrstr(cached_url, "/cache/");
    if (!cache_pos || osStrlen(cache_pos) < 8 + osStrlen("/cache/"))
        return NULL;
    cache_pos += osStrlen("/cache/");

    char hash_str[9] = {0};
    osStrncpy(hash_str, cache_pos, 8);
    uint32_t hash = (uint32_t)osStrtoul(hash_str, NULL, 16);

    cache_entry_t *entry = osAllocMem(sizeof(cache_entry_t));
    if (!entry)
        return NULL;

    entry->next = NULL;
    entry->hash = hash;
    entry->statusCode = 0;
    entry->exists = false;
    entry->original_url = strdup(original_url);
    entry->cached_url = strdup(cached_url);
    entry->file_path = NULL;

    if (!entry->original_url || !entry->cached_url)
    {
        osFreeMem((void *)entry->original_url);
        osFreeMem((void *)entry->cached_url);
        osFreeMem(entry);
        return NULL;
    }

    cache_entry_add(entry);
    return entry;
}

bool cache_fetch_entry(cache_entry_t *entry)
{
    if (entry->exists && fsFileExists(entry->file_path))
    {
        return true;
    }

    error_t err = web_download(entry->original_url, entry->file_path, &entry->statusCode);
    entry->exists = (err == NO_ERROR);

    return entry->exists;
}

cache_entry_t *cache_fetch_by_url(const char *url)
{
    if (url == NULL)
    {
        TRACE_ERROR("URL is NULL\r\n");
        return NULL;
    }

    cache_entry_t *pos = &cache_table;

    while (pos != NULL)
    {
        if (pos->original_url && osStrcmp(pos->original_url, url) == 0)
        {
            TRACE_DEBUG("Cache entry found for URL: %s\r\n", url);
            cache_fetch_entry(pos);
            return pos;
        }

        pos = pos->next;
    }

    TRACE_DEBUG("No cache entry found for URL: %s\r\n", url);
    return NULL;
}

cache_entry_t *cache_fetch_by_cached_url(const char *cached_url)
{
    if (cached_url == NULL)
    {
        TRACE_ERROR("cached_url is NULL\r\n");
        return NULL;
    }

    /* Find the position of "/cache/" in the URL */
    const char *cache_pos = osStrstr(cached_url, "/cache/");
    if (!cache_pos)
    {
        TRACE_ERROR("'/cache/' not found in cached URL: %s\r\n", cached_url);
        return NULL;
    }

    cache_pos += osStrlen("/cache/");

    if (osStrlen(cache_pos) < 8)
    {
        TRACE_ERROR("Cached URL hash is too short in URL: %s\r\n", cached_url);
        return NULL;
    }

    /* Extract the first 8 characters of the hash from the URL */
    char hash_str[9] = {0};
    osStrncpy(hash_str, cache_pos, 8);

    /* Convert the extracted hash part to a uint32_t */
    uint32_t hash_from_url = (uint32_t)osStrtoul(hash_str, NULL, 16);

    cache_entry_t *pos = &cache_table;

    while (pos != NULL)
    {
        if (pos->hash == hash_from_url)
        {
            TRACE_INFO("Hash match found for hash: %08X. Checking full cached URL...\r\n", hash_from_url);

            /* Compare the full cached URL */
            if (strcmp(pos->cached_url, cached_url) == 0)
            {
                TRACE_INFO("Full cached URL match found for URL: %s\r\n", cached_url);
                cache_fetch_entry(pos);
                return pos;
            }
        }

        pos = pos->next;
    }

    TRACE_INFO("No cache entry found for hash: %08X in cached URL: %s\r\n", hash_from_url, cached_url);
    return NULL;
}

cache_entry_t *cache_fetch_by_path(const char *path)
{
    if (path == NULL)
    {
        TRACE_ERROR("URI is NULL\r\n");
        return NULL;
    }

    // Find the position of "/cache/" in the URI
    const char *cache_pos = strstr(path, "/cache/");
    if (!cache_pos)
    {
        TRACE_ERROR("'/cache/' not found in URI: %s\r\n", path);
        return NULL;
    }

    // Move the pointer to the start of the hash part
    cache_pos += osStrlen("/cache/");

    // Ensure that the hash part exists and has enough characters
    if (osStrlen(cache_pos) < 8) // 4 bytes of hash = 8 hex characters
    {
        TRACE_ERROR("URI hash is too short in URI: %s\r\n", path);
        return NULL;
    }

    // Extract the first 8 characters of the hash from the URI
    char hash_str[9] = {0}; // 8 characters + 1 for null terminator
    osStrncpy(hash_str, cache_pos, 8);

    // Convert the extracted hash part to a uint32_t
    uint32_t hash_from_uri = (uint32_t)osStrtoul(hash_str, NULL, 16);

    cache_entry_t *pos = &cache_table;

    while (pos != NULL)
    {
        if (pos->hash == hash_from_uri)
        {
            TRACE_DEBUG("Hash match found for hash: %08X. Checking full URI...\r\n", hash_from_uri);

            // Compare the path "/cache/[hash].[ext]" in the URI
            if (osStrstr(pos->cached_url, cache_pos) != NULL)
            {
                TRACE_DEBUG("Full URI match found for URI: %s\r\n", path);
                cache_fetch_entry(pos);
                return pos;
            }
        }

        pos = pos->next;
    }

    TRACE_ERROR("No cache entry found for URI: %s\r\n", path);
    return NULL;
}

bool cache_get_file_path_for_uri(const char *uri, char **file_path_out)
{
    if (!uri || !file_path_out)
        return false;
    *file_path_out = NULL;

    const char *cache_pos = osStrstr(uri, "/cache/");
    if (!cache_pos)
        return false;
    cache_pos += osStrlen("/cache/");

    if (osStrlen(cache_pos) < 9) /* min: 8-char hash + dot + 1-char ext */
        return false;

    /* Reject path traversal: filename must not contain / or \ */
    for (const char *p = cache_pos; *p; p++)
    {
        if (*p == '/' || *p == '\\')
            return false;
    }

    const char *cachePath = get_settings()->internal.cachedirfull;
    if (!cachePath || !fsDirExists(cachePath))
        return false;

    char *path = custom_asprintf("%s%c%s", cachePath, PATH_SEPARATOR, cache_pos);
    if (!path)
        return false;

    if (!fsFileExists(path))
    {
        osFreeMem(path);
        return false;
    }

    *file_path_out = path;
    return true;
}