#include "tb2_client_identity.h"

#include "debug.h"
#include "net_config.h"
#include "os_port.h"

static const char *const tb2_identity_options[] = {
    "core.client_cert_tb2.file.ca",
    "core.client_cert_tb2.file.crt",
    "core.client_cert_tb2.file.key",
    "core.client_cert_tb2.data.ca",
    "core.client_cert_tb2.data.crt",
    "core.client_cert_tb2.data.key",
};

static bool_t tb2_identity_is_complete(const settings_cert_t *identity)
{
    return identity != NULL && identity->ca != NULL && identity->crt != NULL &&
           identity->key != NULL && identity->ca[0] != '\0' && identity->crt[0] != '\0' &&
           identity->key[0] != '\0';
}

static bool_t tb2_identity_has_explicit_overlay(const settings_t *settings)
{
    if (settings == NULL || settings->internal.overlayNumber == 0 ||
        settings->toniebox.boxGeneration != GENERATION_TB2)
    {
        return FALSE;
    }

    for (size_t index = 0; index < sizeof(tb2_identity_options) / sizeof(tb2_identity_options[0]);
         index++)
    {
        if (settings_is_overlayed_id(tb2_identity_options[index],
                                     settings->internal.overlayNumber))
        {
            return TRUE;
        }
    }
    return FALSE;
}

const settings_cert_t *tb2_client_identity_resolve(settings_t *box_settings,
                                                   const char **source)
{
    const bool_t explicit_overlay = tb2_identity_has_explicit_overlay(box_settings);
    const settings_cert_t *identity = explicit_overlay
                                          ? &box_settings->internal.client_tb2
                                          : &get_settings()->internal.client_tb2;
    const char *identity_source = explicit_overlay ? "box_overlay" : "global_default";

    if (source != NULL)
    {
        *source = identity_source;
    }

    if (!tb2_identity_is_complete(identity))
    {
        if (explicit_overlay)
        {
            TRACE_ERROR("Incomplete explicit TB2 client certificate override for overlay %u; refusing global fallback\r\n",
                        (unsigned int)box_settings->internal.overlayNumber);
        }
        else
        {
            TRACE_ERROR("Global TB2 client certificate set is incomplete\r\n");
        }
        return NULL;
    }

    TRACE_DEBUG("Selected TB2 upstream client identity source=%s overlay=%u\r\n",
                identity_source,
                box_settings != NULL ? (unsigned int)box_settings->internal.overlayNumber : 0U);
    return identity;
}

bool_t tb2_content_identity_resolve(const char *requested_ruid,
                                    const uint8_t *requested_auth,
                                    const contentJson_t *content,
                                    tb2_content_identity_t *identity)
{
    if (identity == NULL)
    {
        return FALSE;
    }
    osMemset(identity, 0, sizeof(*identity));

    const bool_t overridden = content != NULL && content->cloud_override;
    const char *selected_ruid = overridden ? content->cloud_ruid : requested_ruid;
    if (!tb2_ruid_canonicalize(selected_ruid, identity->ruid))
    {
        return FALSE;
    }

    identity->overridden = overridden;
    if (overridden)
    {
        if (!content->_has_cloud_auth || content->cloud_auth == NULL ||
            content->cloud_auth_len != TONIE_AUTH_TOKEN_LENGTH)
        {
            return FALSE;
        }
        identity->auth = content->cloud_auth;
        identity->auth_len = content->cloud_auth_len;
    }
    else if (requested_auth != NULL)
    {
        identity->auth = requested_auth;
        identity->auth_len = TONIE_AUTH_TOKEN_LENGTH;
    }
    return TRUE;
}
