#pragma once

#include <stddef.h>

#include "error.h"
#include "fs_port.h"
#include "os_port.h"

#define TONIE_USER_RUID_LENGTH 16U
#define TONIE_USER_COMMENT_MAX_CHARACTERS 255U
#define TONIE_USER_IMAGE_MAX_SIZE (5U * 1024U * 1024U)

error_t tonie_user_canonicalize_ruid(const char *ruid,
                                     char canonical[TONIE_USER_RUID_LENGTH + 1U]);

error_t tonie_user_comment_validate_and_copy(const char *comment,
                                             char **normalized_comment);
error_t tonie_user_comment_load(const char *config_dir, const char *ruid,
                                char **comment);
error_t tonie_user_comment_save(const char *config_dir, const char *ruid,
                                const char *comment);

char *tonie_user_image_path(const char *www_dir, const char *ruid,
                            bool_t temporary);
const char *tonie_user_image_detect_mime(const char *path);
bool_t tonie_user_image_is_valid(const char *path);
