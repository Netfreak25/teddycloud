#pragma once

#include "settings.h"

/** Read-only content/custom/model selection. Caller owns the returned relative or absolute URL. */
char *tonie_picture_resolve(settings_t *settings, const char *ruid, uint32_t audio_id);
bool_t tonie_picture_is_unknown(const char *picture);
