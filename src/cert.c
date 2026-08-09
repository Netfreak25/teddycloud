
#include "debug.h"
#include "error.h"
#include "fs_port.h"
#include "fs_ext.h"
#include "os_ext.h"
#include "rsa.h"
#include "rand.h"
#include "pem_import.h"
#include "pem_export.h"
#include "x509_cert_parse.h"
#include "x509_cert_create.h"
#include "x509_key_format.h"
#include "encoding/asn1.h"
#include "ecc/ec.h"
#include "ecc/ec_curves.h"
#include "ecc/ecdsa.h"
#include "pkcs8_key_format.h"
#include "x509_key_parse.h"
#include "server_helpers.h"
#include "x509_cert_validate.h"
#include "hash/sha256.h"
#include "date_time.h"

#include "tls_adapter.h"
#include "settings.h"
#include "cert.h"
#include "mqtt_server.h"

#define TB2_CERT_PATH_SIZE 512
#define TB2_CERT_NEAR_EXPIRY_SECONDS (24 * 60 * 60)

static OsMutex tb2_cert_rotation_mutex;
static bool_t tb2_cert_rotation_mutex_ready = FALSE;

void cert_tb2_rotation_lock(void)
{
    if (!tb2_cert_rotation_mutex_ready)
    {
        tb2_cert_rotation_mutex_ready = osCreateMutex(&tb2_cert_rotation_mutex);
    }
    if (tb2_cert_rotation_mutex_ready)
    {
        osAcquireMutex(&tb2_cert_rotation_mutex);
    }
}

void cert_tb2_rotation_unlock(void)
{
    if (tb2_cert_rotation_mutex_ready)
    {
        osReleaseMutex(&tb2_cert_rotation_mutex);
    }
}

static int hex2int(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

static void hex_string_to_bytes(const char *hex_string, uint8_t *output)
{
    while (*hex_string)
    {
        char hi = hex2int(*hex_string++);
        char lo = hex2int(*hex_string++);
        *output++ = (hi << 4) | lo;
    }
}

error_t cert_generate_rsa(int size, RsaPrivateKey *cert_privkey, RsaPublicKey *cert_pubkey)
{
    TRACE_INFO("Generating RSA Key... (slow, very slow!!!)\r\n");

    osMemset(cert_privkey, 0x00, sizeof(RsaPrivateKey));
    osMemset(cert_pubkey, 0x00, sizeof(RsaPublicKey));

    if (rsaGenerateKeyPair(rand_get_algo(), rand_get_context(), size, 65537, cert_privkey, cert_pubkey) != NO_ERROR)
    {
        TRACE_ERROR("rsaGenerateKeyPair failed\r\n");
        return ERROR_FAILURE;
    }
    return NO_ERROR;
}

error_t cert_get_rsa_priv(RsaPrivateKey *cert_privkey, uint8_t **priv_data, size_t *priv_size)
{
    if (x509ExportRsaPrivateKey(cert_privkey, NULL, priv_size) != NO_ERROR)
    {
        TRACE_ERROR("x509ExportRsaPrivateKey failed\r\n");
        return ERROR_FAILURE;
    }

    *priv_data = osAllocMem(*priv_size);

    if (x509ExportRsaPrivateKey(cert_privkey, *priv_data, priv_size) != NO_ERROR)
    {
        TRACE_ERROR("x509ExportRsaPrivateKey failed\r\n");
        return ERROR_FAILURE;
    }
    return NO_ERROR;
}

static error_t cert_load_ca_data(const char *server_ca, const char *server_key,
                                 X509CertInfo *cert, RsaPrivateKey *cert_priv)
{
    size_t ca_size = 0;
    TRACE_INFO("Load CA certificate...\r\n");
    if (pemImportCertificate(server_ca, strlen(server_ca), NULL, &ca_size, NULL) != NO_ERROR)
    {
        TRACE_ERROR("pemImportCertificate failed\r\n");
        return ERROR_FAILURE;
    }

    uint8_t *server_ca_der = osAllocMem(ca_size);
    if (pemImportCertificate(server_ca, strlen(server_ca), server_ca_der, &ca_size, NULL) != NO_ERROR)
    {
        TRACE_ERROR("pemImportCertificate failed\r\n");
        return ERROR_FAILURE;
    }

    osMemset(cert, 0x00, sizeof(X509CertInfo));
    if (x509ParseCertificateEx(server_ca_der, ca_size, cert, true) != NO_ERROR)
    {
        TRACE_ERROR("x509ParseCertificateEx failed\r\n");
        return ERROR_FAILURE;
    }

    /* now export private key */
    osMemset(cert_priv, 0x00, sizeof(RsaPrivateKey));

    TRACE_INFO("Load CA key...\r\n");
    if (pemImportRsaPrivateKey(server_key, osStrlen(server_key), NULL, cert_priv) != NO_ERROR)
    {
        TRACE_ERROR("pemImportRsaPrivateKey failed\r\n");
        return ERROR_FAILURE;
    }

    /* we must not free this DER because the parsed certificate seems to point there */
    // osFreeMem(server_ca_der);

    return NO_ERROR;
}

error_t cert_load_ca(X509CertInfo *cert, RsaPrivateKey *cert_priv)
{
    return cert_load_ca_data(settings_get_string("internal.server.ca"),
                             settings_get_string("internal.server.ca_key"),
                             cert, cert_priv);
}

static error_t cert_generate_signed_with_issuer(
    const char *subject, const uint8_t *serial_number, int serial_number_size,
    size_t key_size, bool self_sign, bool cert_der_format,
    const char *cert_file, const char *priv_file,
    const char *issuer_cert_file, const char *issuer_key_file)
{
    /* load server CA certificate */
    X509CertInfo issuer_cert;
    RsaPrivateKey issuer_priv;

    if (!self_sign)
    {
        char *issuer_cert_data = NULL;
        char *issuer_key_data = NULL;
        size_t issuer_cert_size = 0;
        size_t issuer_key_size = 0;
        error_t issuer_error = NO_ERROR;
        if (issuer_cert_file != NULL && issuer_key_file != NULL)
        {
            issuer_error = read_certificate(issuer_cert_file, &issuer_cert_data,
                                            &issuer_cert_size);
            if (issuer_error == NO_ERROR)
            {
                issuer_error = read_certificate(issuer_key_file, &issuer_key_data,
                                                &issuer_key_size);
            }
            if (issuer_error == NO_ERROR)
            {
                issuer_error = cert_load_ca_data(issuer_cert_data, issuer_key_data,
                                                 &issuer_cert, &issuer_priv);
            }
        }
        else
        {
            issuer_error = cert_load_ca(&issuer_cert, &issuer_priv);
        }
        osFreeMem(issuer_cert_data);
        osFreeMem(issuer_key_data);
        if (issuer_error != NO_ERROR)
        {
            TRACE_ERROR("cert_load_ca failed\r\n");
            return ERROR_FAILURE;
        }
    }

    /* generate RSA key */
    RsaPrivateKey cert_privkey;
    RsaPublicKey cert_pubkey;
    size_t priv_size = 0;
    uint8_t *priv_data = NULL;

    if (cert_generate_rsa(key_size, &cert_privkey, &cert_pubkey) != NO_ERROR)
    {
        TRACE_ERROR("cert_generate_rsa failed\r\n");
        return ERROR_FAILURE;
    }
    if (cert_get_rsa_priv(&cert_privkey, &priv_data, &priv_size) != NO_ERROR)
    {
        TRACE_ERROR("cert_get_rsa_priv failed\r\n");
        return ERROR_FAILURE;
    }

    /* #115: if the private-key file uses a .pem/.key extension, also export the key
       as PEM (PKCS#8) so we write a real PEM file instead of raw DER. This must run
       while cert_privkey is still valid (it is freed below after signing); .der
       targets keep DER. */
    char_t *priv_pem_data = NULL;
    size_t priv_pem_size = 0;
    size_t priv_file_len = priv_file ? osStrlen(priv_file) : 0;
    if (priv_file_len > 4 && (!osStrcasecmp(&priv_file[priv_file_len - 4], ".pem") || !osStrcasecmp(&priv_file[priv_file_len - 4], ".key")))
    {
        /* Export into a fixed buffer, mirroring cert_generate_signed_ec. 8 KB holds
           a 4096-bit RSA key in PKCS#8 PEM with room to spare. (pemExportRsaPrivateKey
           must not be called with a NULL output buffer.) */
        char_t temp_pem_buf[8192];
        size_t temp_pem_size = sizeof(temp_pem_buf);
        if (pemExportRsaPrivateKey(&cert_privkey, temp_pem_buf, &temp_pem_size) == NO_ERROR && temp_pem_size > 0)
        {
            priv_pem_data = osAllocMem(temp_pem_size + 1);
            osMemcpy(priv_pem_data, temp_pem_buf, temp_pem_size);
            priv_pem_data[temp_pem_size] = '\0';
            priv_pem_size = temp_pem_size;
        }
        else
        {
            TRACE_WARNING("PEM export of RSA private key failed; writing DER to '%s'\r\n", priv_file);
        }
    }

    /* create and sign the certificate */
    X509CertRequestInfo cert_req;
    osMemset(&cert_req, 0x00, sizeof(cert_req));
    cert_req.version = X509_VERSION_1;
    cert_req.subject.name.value = subject;
    cert_req.subject.name.length = osStrlen(subject);
    cert_req.subject.commonName.value = subject;
    cert_req.subject.commonName.length = osStrlen(subject);
    cert_req.subject.organizationName.value = "Team RevvoX";
    cert_req.subject.organizationName.length = 11;
    cert_req.subject.countryName.value = "DE";
    cert_req.subject.countryName.length = 2;
    cert_req.subject.localityName.value = "Duesseldorf";
    cert_req.subject.localityName.length = 11;
    cert_req.subject.stateOrProvinceName.value = "NW";
    cert_req.subject.stateOrProvinceName.length = 2;

    cert_req.subjectPublicKeyInfo.oid.value = RSA_ENCRYPTION_OID;
    cert_req.subjectPublicKeyInfo.oid.length = sizeof(RSA_ENCRYPTION_OID);

    /*
    cert_req.attributes.extensionReq.keyUsage.bitmap |= X509_KEY_USAGE_DIGITAL_SIGNATURE;
    cert_req.attributes.extensionReq.keyUsage.bitmap |= X509_KEY_USAGE_NON_REPUDIATION;
    cert_req.attributes.extensionReq.extKeyUsage.bitmap |= X509_EXT_KEY_USAGE_SERVER_AUTH;
    cert_req.attributes.extensionReq.extKeyUsage.bitmap |= X509_EXT_KEY_USAGE_CLIENT_AUTH;
    */

    if (self_sign)
    {
        cert_req.attributes.extensionReq.basicConstraints.cA = true;
        // cert_req.attributes.extensionReq.keyUsage.bitmap |= X509_KEY_USAGE_KEY_CERT_SIGN;
    }

    X509SerialNumber serial;
    osMemset(&serial, 0x00, sizeof(serial));
    serial.length = serial_number_size;
    serial.value = serial_number;

    X509Validity validity;
    osMemset(&validity, 0x00, sizeof(validity));
    getCurrentDate(&validity.notBefore);
    getCurrentDate(&validity.notAfter);

    validity.notBefore.year = 2015;
    validity.notBefore.month = 11;
    validity.notBefore.day = 3;
    validity.notBefore.hours = 15;
    validity.notBefore.minutes = 23;
    validity.notBefore.seconds = 19;

    validity.notAfter.year = 2040;
    validity.notAfter.month = 6;
    validity.notAfter.day = 24;
    validity.notAfter.hours = 15;
    validity.notAfter.minutes = 23;
    validity.notAfter.seconds = 19;

    X509SignAlgoId algo;
    osMemset(&algo, 0x00, sizeof(algo));
    algo.oid.value = SHA256_WITH_RSA_ENCRYPTION_OID;
    algo.oid.length = sizeof(SHA256_WITH_RSA_ENCRYPTION_OID);

    /* create certificate */
    uint8_t *cert_der_data = osAllocMem(8192);
    size_t cert_der_size = 0;
    error_t error = x509CreateCertificate(rand_get_algo(), rand_get_context(), &cert_req, &cert_pubkey, self_sign ? NULL : &issuer_cert, &serial, &validity, &algo, self_sign ? &cert_privkey : &issuer_priv, cert_der_data, &cert_der_size);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("x509CreateCertificate failed: %s\r\n", error2text(error));
        return ERROR_FAILURE;
    }

    rsaFreePublicKey(&cert_pubkey);
    rsaFreePrivateKey(&cert_privkey);

    /* export certificate */
    size_t cert_pem_size;
    if (pemExportCertificate(cert_der_data, cert_der_size, NULL, &cert_pem_size) != NO_ERROR)
    {
        TRACE_ERROR("pemExportCertificate failed\r\n");
        return ERROR_FAILURE;
    }

    char_t *cert_pem_data = osAllocMem(cert_pem_size + 1);
    if (pemExportCertificate(cert_der_data, cert_der_size, cert_pem_data, &cert_pem_size) != NO_ERROR)
    {
        TRACE_ERROR("pemExportCertificate failed\r\n");
        return ERROR_FAILURE;
    }

    if (cert_file)
    {
        char *cert_file_full = osAllocMem(256);
        settings_resolve_dir(&cert_file_full, (char *)cert_file, get_settings()->internal.basedirfull);

        /* save the cert as pem */
        FsFile *file = fsOpenFile(cert_file_full, FS_FILE_MODE_WRITE);
        osFreeMem(cert_file_full);
        if (!file)
        {
            TRACE_ERROR("fsOpenFile failed\r\n");
            return ERROR_FAILURE;
        }
        if (!cert_der_format)
        {
            fsWriteFile(file, cert_pem_data, cert_pem_size);
        }
        else
        {
            fsWriteFile(file, cert_der_data, cert_der_size);
        }
        fsCloseFile(file);
    }

    if (priv_file)
    {
        char *priv_file_full = osAllocMem(256);
        settings_resolve_dir(&priv_file_full, (char *)priv_file, get_settings()->internal.basedirfull);

        /* save the private key */
        FsFile *file = fsOpenFile(priv_file_full, FS_FILE_MODE_WRITE);
        osFreeMem(priv_file_full);
        if (!file)
        {
            TRACE_ERROR("fsOpenFile failed\r\n");
            return ERROR_FAILURE;
        }
        if (priv_pem_data)
        {
            fsWriteFile(file, priv_pem_data, priv_pem_size);
        }
        else
        {
            fsWriteFile(file, priv_data, priv_size);
        }
        fsCloseFile(file);
    }

    osFreeMem(cert_der_data);
    osFreeMem(cert_pem_data);
    osFreeMem(priv_data);
    if (priv_pem_data) osFreeMem(priv_pem_data);

    if (!self_sign)
    {
        rsaFreePrivateKey(&issuer_priv);
    }

    return NO_ERROR;
}

error_t cert_generate_signed(const char *subject, const uint8_t *serial_number,
                             int serial_number_size, size_t key_size, bool self_sign,
                             bool cert_der_format, const char *cert_file,
                             const char *priv_file)
{
    return cert_generate_signed_with_issuer(subject, serial_number,
                                            serial_number_size, key_size,
                                            self_sign, cert_der_format,
                                            cert_file, priv_file, NULL, NULL);
}

error_t cert_generate_mac(const char *mac, const char *dest)
{
    if (!dest || osStrlen(mac) != 12)
    {
        return ERROR_FAILURE;
    }

    uint8_t serial[7];
    size_t serial_length = 7;
    char_t subj[32];

    serial[0] = 0;
    hex_string_to_bytes(mac, &serial[1]);
    cert_truncate_serial(serial, &serial_length);

    osSprintf(subj, "b'%s'", mac);

    char_t *client_file = custom_asprintf("%s/client.der", dest);
    char_t *private_file = custom_asprintf("%s/private.der", dest);

    if (cert_generate_signed(subj, serial, 7, CERT_RSA_SIZE, false, true, client_file, private_file) != NO_ERROR)
    {
        TRACE_ERROR("cert_generate_signed failed\r\n");
        return ERROR_FAILURE;
    }
    osFreeMem(client_file);
    osFreeMem(private_file);

    return NO_ERROR;
}

void cert_truncate_serial(uint8_t *serial, size_t *serial_length)
{
    /* skip leadin zeroes, except if the next byte is > 127 */
    while (*serial_length > 1)
    {
        /* only skip leading zeroes */
        if (serial[0])
        {
            break;
        }
        /* only allow leading zeroes if the next byte would have highest bit set */
        if (serial[1] & 0x80)
        {
            break;
        }
        (*serial_length)--;
        osMemmove(&serial[0], &serial[1], *serial_length);
    }
}

void cert_generate_serial(uint8_t *serial, size_t *serial_length)
{
    time_t cur_time = getCurrentUnixTime();

    /* write the current time in big endian format with leading zero */
    //*serial_length = 18 + 1;
    serial[0] = 0;
    STORE64BE(cur_time, &serial[1]);

    /* now truncate the 9 byte BE buffer to no leading zeroes, except the number would be interpreted as negative */
    cert_truncate_serial(serial, serial_length);
}

error_t convert_PEM_to_DER(const char *pem_data, const char *der_target_file)
{
    size_t pem_data_len = strlen(pem_data);

    // Call pemDecodeFile to get the DER data size
    size_t der_data_len = 0;
    PemHeader pem_header;
    size_t consumed;
    error_t error = pemDecodeFile(pem_data, pem_data_len, "CERTIFICATE", NULL, &der_data_len, &pem_header, &consumed);

    if (error != NO_ERROR)
    {
        TRACE_ERROR("Error: Unable to decode PEM data for size.\r\n");
        return error;
    }

    // Allocate memory for the DER data
    uint8_t *der_data = osAllocMem(der_data_len);
    if (!der_data)
    {
        TRACE_ERROR("Error: Memory allocation failed.\r\n");
        return ERROR_OUT_OF_MEMORY;
    }

    // Call pemDecodeFile again to get the DER data
    error = pemDecodeFile(pem_data, pem_data_len, "CERTIFICATE", der_data, &der_data_len, &pem_header, &consumed);

    if (error != NO_ERROR)
    {
        TRACE_ERROR("Error: Unable to decode PEM data.\r\n");
        osFreeMem(der_data);
        return error;
    }

    // Open the DER file for writing
    char *der_target_file_full = osAllocMem(256);
    settings_resolve_dir(&der_target_file_full, (char *)der_target_file, get_settings()->internal.basedirfull);
    FsFile *der_file = fsOpenFile(der_target_file_full, FS_FILE_MODE_WRITE);
    osFreeMem(der_target_file_full);
    if (!der_file)
    {
        TRACE_ERROR("Error opening DER file for writing.\r\n");
        osFreeMem(der_data);
        return ERROR_FILE_OPENING_FAILED;
    }

    // Write DER content to the file
    error = fsWriteFile(der_file, der_data, der_data_len);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("Error writing DER data to file.\r\n");
        fsCloseFile(der_file);
        osFreeMem(der_data);
        return error;
    }

    // Close the file
    fsCloseFile(der_file);

    // Clean up
    osFreeMem(der_data);

    return NO_ERROR;
}

static char *cert_resolve_configured_path(const char *setting)
{
    const char *configured = settings_get_string(setting);
    char *resolved = osAllocMem(TB2_CERT_PATH_SIZE);
    if (resolved == NULL)
        return NULL;
    resolved[0] = '\0';
    settings_resolve_dir(&resolved, (char *)configured,
                         get_settings()->internal.basedirfull);
    return resolved;
}

static bool_t cert_path_is_nonempty(const char *path)
{
    FsFileStat stat;
    return path != NULL && fsGetFileStat(path, &stat) == NO_ERROR && stat.size > 0;
}

static error_t cert_prepare_parent(const char *path)
{
    char parent[TB2_CERT_PATH_SIZE];
    osSnprintf(parent, sizeof(parent), "%s", path);
    fsRemoveFilename(parent);
    error_t error = fsCreateDirEx(parent, TRUE);
    return error == NO_ERROR || fsDirExists(parent) ? NO_ERROR : error;
}

static error_t cert_publish_new_file(const char *temporary, const char *target,
                                     bool_t *published)
{
    if (!cert_path_is_nonempty(temporary) || fsFileExists(target))
        return ERROR_ABORTED;
    error_t error = fsMoveFile(temporary, target, FALSE);
    if (error == NO_ERROR)
        *published = TRUE;
    return error;
}

error_t cert_generate_default()
{
    char *ca = cert_resolve_configured_path("core.server_cert.file.ca");
    char *caDer = cert_resolve_configured_path("core.server_cert.file.ca_der");
    char *caKey = cert_resolve_configured_path("core.server_cert.file.ca_key");
    char *leaf = cert_resolve_configured_path("core.server_cert.file.crt");
    char *leafKey = cert_resolve_configured_path("core.server_cert.file.key");
    if (ca == NULL || caDer == NULL || caKey == NULL || leaf == NULL || leafKey == NULL)
    {
        osFreeMem(ca);
        osFreeMem(caDer);
        osFreeMem(caKey);
        osFreeMem(leaf);
        osFreeMem(leafKey);
        return ERROR_OUT_OF_MEMORY;
    }

    bool_t caExists = cert_path_is_nonempty(ca);
    bool_t caKeyExists = cert_path_is_nonempty(caKey);
    bool_t caDerExists = cert_path_is_nonempty(caDer);
    bool_t leafExists = cert_path_is_nonempty(leaf);
    bool_t leafKeyExists = cert_path_is_nonempty(leafKey);
    TRACE_INFO("TB1 certificate initialization paths ca='%s' ca_der='%s' ca_key='%s' cert='%s' key='%s'\r\n",
               ca, caDer, caKey, leaf, leafKey);

    error_t error = NO_ERROR;
    if (caExists != caKeyExists)
    {
        TRACE_ERROR("TB1 certificate set is unsafe: CA certificate/key pair is incomplete; no files changed\r\n");
        error = ERROR_ABORTED;
        goto cleanup;
    }
    if (leafExists != leafKeyExists)
    {
        TRACE_ERROR("TB1 certificate set is unsafe: server certificate/key pair is incomplete; no files changed\r\n");
        error = ERROR_ABORTED;
        goto cleanup;
    }
    if (!caExists && (caDerExists || leafExists || leafKeyExists))
    {
        TRACE_ERROR("TB1 certificate set is unsafe: dependent files exist without a complete CA pair; no files changed\r\n");
        error = ERROR_ABORTED;
        goto cleanup;
    }

    char *caNew = custom_asprintf("%s.new", ca);
    char *caDerNew = custom_asprintf("%s.new", caDer);
    char *caKeyNew = custom_asprintf("%s.new", caKey);
    char *leafNew = custom_asprintf("%s.new", leaf);
    char *leafKeyNew = custom_asprintf("%s.new", leafKey);
    if (caNew == NULL || caDerNew == NULL || caKeyNew == NULL ||
        leafNew == NULL || leafKeyNew == NULL)
    {
        error = ERROR_OUT_OF_MEMORY;
        osFreeMem(caNew);
        osFreeMem(caDerNew);
        osFreeMem(caKeyNew);
        osFreeMem(leafNew);
        osFreeMem(leafKeyNew);
        goto cleanup;
    }

    fsDeleteFile(caNew);
    fsDeleteFile(caDerNew);
    fsDeleteFile(caKeyNew);
    fsDeleteFile(leafNew);
    fsDeleteFile(leafKeyNew);
    error = cert_prepare_parent(ca);
    if (error != NO_ERROR)
        goto staged_cleanup;

    uint8_t serial[14];
    size_t serialLength = sizeof(serial);
    if (!caExists)
    {
        cert_generate_serial(serial, &serialLength);
        TRACE_INFO("Generating missing TB1 CA pair in staging\r\n");
        error = cert_generate_signed("TeddyCloud CA Root Cert.", serial,
                                     serialLength, CA_RSA_SIZE, true, false,
                                     caNew, caKeyNew);
        if (error != NO_ERROR)
            goto staged_cleanup;
    }

    const char *effectiveCa = caExists ? ca : caNew;
    const char *effectiveCaKey = caKeyExists ? caKey : caKeyNew;
    if (!caDerExists)
    {
        char *caData = NULL;
        size_t caDataLength = 0;
        error = read_certificate(effectiveCa, &caData, &caDataLength);
        if (error == NO_ERROR)
            error = convert_PEM_to_DER(caData, caDerNew);
        osFreeMem(caData);
        if (error != NO_ERROR)
            goto staged_cleanup;
    }

    if (!leafExists)
    {
        serialLength = sizeof(serial);
        cert_generate_serial(serial, &serialLength);
        TRACE_INFO("Generating missing TB1 server certificate pair in staging\r\n");
        error = cert_generate_signed_with_issuer(
            "TeddyCloud Server", serial, serialLength, CERT_RSA_SIZE,
            false, false, leafNew, leafKeyNew, effectiveCa, effectiveCaKey);
        if (error != NO_ERROR)
            goto staged_cleanup;
    }

    const char *effectiveCaDer = caDerExists ? caDer : caDerNew;
    const char *effectiveLeaf = leafExists ? leaf : leafNew;
    const char *effectiveLeafKey = leafKeyExists ? leafKey : leafKeyNew;
    char validation[192];
    error = cert_validate_server_files(effectiveCa, effectiveCaDer, effectiveCaKey,
                                       effectiveLeaf, effectiveLeafKey,
                                       NULL, 0, FALSE, validation,
                                       sizeof(validation));
    if (error != NO_ERROR)
    {
        TRACE_ERROR("TB1 certificate staging validation failed: %s; no files changed\r\n",
                    validation);
        goto staged_cleanup;
    }

    bool_t publishedCa = FALSE;
    bool_t publishedCaKey = FALSE;
    bool_t publishedCaDer = FALSE;
    bool_t publishedLeaf = FALSE;
    bool_t publishedLeafKey = FALSE;
    if (!caExists)
        error = cert_publish_new_file(caNew, ca, &publishedCa);
    if (error == NO_ERROR && !caKeyExists)
        error = cert_publish_new_file(caKeyNew, caKey, &publishedCaKey);
    if (error == NO_ERROR && !caDerExists)
        error = cert_publish_new_file(caDerNew, caDer, &publishedCaDer);
    if (error == NO_ERROR && !leafExists)
        error = cert_publish_new_file(leafNew, leaf, &publishedLeaf);
    if (error == NO_ERROR && !leafKeyExists)
        error = cert_publish_new_file(leafKeyNew, leafKey, &publishedLeafKey);
    if (error != NO_ERROR)
    {
        if (publishedLeafKey) fsDeleteFile(leafKey);
        if (publishedLeaf) fsDeleteFile(leaf);
        if (publishedCaDer) fsDeleteFile(caDer);
        if (publishedCaKey) fsDeleteFile(caKey);
        if (publishedCa) fsDeleteFile(ca);
        TRACE_ERROR("TB1 certificate publication failed and newly published files were rolled back: %s\r\n",
                    error2text(error));
        goto staged_cleanup;
    }

    error = cert_validate_server_files(ca, caDer, caKey, leaf, leafKey,
                                       NULL, 0, FALSE, validation,
                                       sizeof(validation));
    if (error != NO_ERROR)
    {
        if (publishedLeafKey) fsDeleteFile(leafKey);
        if (publishedLeaf) fsDeleteFile(leaf);
        if (publishedCaDer) fsDeleteFile(caDer);
        if (publishedCaKey) fsDeleteFile(caKey);
        if (publishedCa) fsDeleteFile(ca);
        TRACE_ERROR("TB1 certificate final validation failed and newly published files were rolled back: %s\r\n",
                    validation);
        goto staged_cleanup;
    }

    TRACE_INFO("TB1 certificate set is complete and valid; existing files were preserved\r\n");
    error = settings_try_load_certs_id(0);

staged_cleanup:
    fsDeleteFile(caNew);
    fsDeleteFile(caDerNew);
    fsDeleteFile(caKeyNew);
    fsDeleteFile(leafNew);
    fsDeleteFile(leafKeyNew);
    osFreeMem(caNew);
    osFreeMem(caDerNew);
    osFreeMem(caKeyNew);
    osFreeMem(leafNew);
    osFreeMem(leafKeyNew);

cleanup:
    osFreeMem(ca);
    osFreeMem(caDer);
    osFreeMem(caKey);
    osFreeMem(leaf);
    osFreeMem(leafKey);
    return error;
}

error_t x509ExportEcPrivateKey(const EcCurveInfo *curveInfo,
   const EcPrivateKey *privateKey, const EcPublicKey *publicKey,
   uint8_t *output, size_t *written)
{
   uint8_t temp_buf[512];
   error_t error;
   size_t n;
   size_t length;
   uint8_t *p;
   Asn1Tag tag;

   p = temp_buf;
   length = 0;

   //Format Version field (refer to RFC 5915, section 3)
   error = asn1WriteInt32(1, FALSE, p, &n);
   if(error) return error;
   length += n;
   p += n;

   //Write the EC private key d as an octet string
   error = mpiWriteRaw(&privateKey->d, p, curveInfo->pLen);
   if(error) return error;

   tag.constructed = FALSE;
   tag.objClass = ASN1_CLASS_UNIVERSAL;
   tag.objType = ASN1_TYPE_OCTET_STRING;
   tag.length = curveInfo->pLen;
   tag.value = p;

   error = asn1WriteTag(&tag, FALSE, p, &n);
   if(error) return error;

   n = tag.totalLength;
   length += n;
   p += n;

   //The parameters field is optional but some parsers require it
   if(curveInfo->oid != NULL)
   {
      //Write the curve OID
      tag.constructed = FALSE;
      tag.objClass = ASN1_CLASS_UNIVERSAL;
      tag.objType = ASN1_TYPE_OBJECT_IDENTIFIER;
      tag.length = curveInfo->oidSize;
      tag.value = curveInfo->oid;

      error = asn1WriteTag(&tag, FALSE, p, &n);
      if(error) return error;

      n = tag.totalLength;

      //Wrap in a context-specific tag [0]
      tag.constructed = TRUE;
      tag.objClass = ASN1_CLASS_CONTEXT_SPECIFIC;
      tag.objType = 0;
      tag.length = n;
      tag.value = p;

      error = asn1WriteTag(&tag, FALSE, p, &n);
      if(error) return error;

      n = tag.totalLength;
      length += n;
      p += n;
   }


   //The public key is optional
   if(publicKey != NULL)
   {
      error = pkcs8FormatEcPublicKey(curveInfo, publicKey, p, &n);
      if(error) return error;
      length += n;
      p += n;
   }

   //Format ECPrivateKey field
   tag.constructed = TRUE;
   tag.objClass = ASN1_CLASS_UNIVERSAL;
   tag.objType = ASN1_TYPE_SEQUENCE;
   tag.length = length;
   tag.value = temp_buf;

   error = asn1WriteTag(&tag, FALSE, temp_buf, &n);
   if(error) return error;

   *written = tag.totalLength;
   if(output != NULL)
   {
      osMemcpy(output, temp_buf, tag.totalLength);
   }
   return NO_ERROR;
}

static error_t cert_load_ca_ec_data(const char *server_ca, const char *server_key,
                                    X509CertInfo *cert, EcPrivateKey *cert_priv,
                                    uint8_t **server_ca_der_out)
{
    if (!server_ca || !server_key)
    {
        TRACE_ERROR("CA certificate or key setting not found\r\n");
        return ERROR_FAILURE;
    }

    size_t ca_size = 0;
    TRACE_INFO("Load CA certificate...\r\n");
    if (pemImportCertificate(server_ca, strlen(server_ca), NULL, &ca_size, NULL) != NO_ERROR)
    {
        TRACE_ERROR("pemImportCertificate failed\r\n");
        return ERROR_FAILURE;
    }

    uint8_t *server_ca_der = osAllocMem(ca_size);
    if (pemImportCertificate(server_ca, strlen(server_ca), server_ca_der, &ca_size, NULL) != NO_ERROR)
    {
        TRACE_ERROR("pemImportCertificate failed\r\n");
        osFreeMem(server_ca_der);
        return ERROR_FAILURE;
    }

    osMemset(cert, 0x00, sizeof(X509CertInfo));
    if (x509ParseCertificateEx(server_ca_der, ca_size, cert, true) != NO_ERROR)
    {
        TRACE_ERROR("x509ParseCertificateEx failed\r\n");
        osFreeMem(server_ca_der);
        return ERROR_FAILURE;
    }

    /* now export private key */
    osMemset(cert_priv, 0x00, sizeof(EcPrivateKey));
    ecInitPrivateKey(cert_priv);

    TRACE_INFO("Load CA key...\r\n");
    if (pemImportEcPrivateKey(server_key, osStrlen(server_key), NULL, cert_priv) != NO_ERROR)
    {
        TRACE_ERROR("pemImportEcPrivateKey failed\r\n");
        osFreeMem(server_ca_der);
        return ERROR_FAILURE;
    }

    if (server_ca_der_out) {
        *server_ca_der_out = server_ca_der;
    } else {
        osFreeMem(server_ca_der);
    }
    return NO_ERROR;
}

error_t cert_load_ca_ec(const char *ca_cert_setting, const char *ca_key_setting,
                        X509CertInfo *cert, EcPrivateKey *cert_priv,
                        uint8_t **server_ca_der_out)
{
    return cert_load_ca_ec_data(settings_get_string(ca_cert_setting),
                                settings_get_string(ca_key_setting), cert,
                                cert_priv, server_ca_der_out);
}

static error_t cert_generate_signed_ec_internal(
    const char *subject,
    const uint8_t *serial_number,
    int serial_number_size,
    bool self_sign,
    bool cert_der_format,
    const char *cert_file,
    const char *priv_file,
    const char *ca_cert_setting,
    const char *ca_key_setting,
    const char *dns_names[],
    size_t dns_names_count,
    const char *issuer_cert_file,
    const char *issuer_key_file
) {
    /* load server CA certificate */
    X509CertInfo issuer_cert;
    EcPrivateKey issuer_priv;
    osMemset(&issuer_cert, 0, sizeof(issuer_cert));
    osMemset(&issuer_priv, 0, sizeof(issuer_priv));
    ecInitPrivateKey(&issuer_priv);

    uint8_t *server_ca_der = NULL;
    if (!self_sign)
    {
        char *issuer_cert_data = NULL;
        char *issuer_key_data = NULL;
        size_t issuer_cert_size = 0;
        size_t issuer_key_size = 0;
        error_t issuer_error = NO_ERROR;
        if (issuer_cert_file != NULL && issuer_key_file != NULL)
        {
            issuer_error = read_certificate(issuer_cert_file, &issuer_cert_data,
                                            &issuer_cert_size);
            if (issuer_error == NO_ERROR)
            {
                issuer_error = read_certificate(issuer_key_file, &issuer_key_data,
                                                &issuer_key_size);
            }
            if (issuer_error == NO_ERROR)
            {
                issuer_error = cert_load_ca_ec_data(issuer_cert_data,
                                                    issuer_key_data,
                                                    &issuer_cert, &issuer_priv,
                                                    &server_ca_der);
            }
        }
        else
        {
            issuer_error = cert_load_ca_ec(ca_cert_setting, ca_key_setting,
                                           &issuer_cert, &issuer_priv,
                                           &server_ca_der);
        }
        osFreeMem(issuer_cert_data);
        osFreeMem(issuer_key_data);
        if (issuer_error != NO_ERROR)
        {
            TRACE_ERROR("cert_load_ca_ec failed\r\n");
            ecFreePrivateKey(&issuer_priv);
            return ERROR_FAILURE;
        }
    }

    /* generate EC key */
    EcPrivateKey cert_privkey;
    EcPublicKey cert_pubkey;
    ecInitPrivateKey(&cert_privkey);
    ecInitPublicKey(&cert_pubkey);
    size_t priv_size = 0;
    uint8_t *priv_data = NULL;

    EcDomainParameters params;
    ecInitDomainParameters(&params);
    if (ecLoadDomainParameters(&params, SECP384R1_CURVE) != NO_ERROR)
    {
        TRACE_ERROR("ecLoadDomainParameters failed\r\n");
        ecFreePrivateKey(&cert_privkey);
        ecFreePublicKey(&cert_pubkey);
        ecFreeDomainParameters(&params);
        if (!self_sign) {
            ecFreePrivateKey(&issuer_priv);
            if (server_ca_der) osFreeMem(server_ca_der);
        }
        return ERROR_FAILURE;
    }

    if (ecGenerateKeyPair(rand_get_algo(), rand_get_context(), &params, &cert_privkey, &cert_pubkey) != NO_ERROR)
    {
        TRACE_ERROR("ecGenerateKeyPair failed\r\n");
        ecFreePrivateKey(&cert_privkey);
        ecFreePublicKey(&cert_pubkey);
        ecFreeDomainParameters(&params);
        if (!self_sign) {
            ecFreePrivateKey(&issuer_priv);
            if (server_ca_der) osFreeMem(server_ca_der);
        }
        return ERROR_FAILURE;
    }

    // Export private key to DER format (SEC-1 ECPrivateKey)
    if (x509ExportEcPrivateKey(SECP384R1_CURVE, &cert_privkey, &cert_pubkey, NULL, &priv_size) != NO_ERROR)
    {
        TRACE_ERROR("x509ExportEcPrivateKey size check failed\r\n");
        ecFreePrivateKey(&cert_privkey);
        ecFreePublicKey(&cert_pubkey);
        ecFreeDomainParameters(&params);
        if (!self_sign) {
            ecFreePrivateKey(&issuer_priv);
            if (server_ca_der) osFreeMem(server_ca_der);
        }
        return ERROR_FAILURE;
    }

    priv_data = osAllocMem(priv_size);
    if (x509ExportEcPrivateKey(SECP384R1_CURVE, &cert_privkey, &cert_pubkey, priv_data, &priv_size) != NO_ERROR)
    {
        TRACE_ERROR("x509ExportEcPrivateKey failed\r\n");
        osFreeMem(priv_data);
        ecFreePrivateKey(&cert_privkey);
        ecFreePublicKey(&cert_pubkey);
        ecFreeDomainParameters(&params);
        if (!self_sign) {
            ecFreePrivateKey(&issuer_priv);
            if (server_ca_der) osFreeMem(server_ca_der);
        }
        return ERROR_FAILURE;
    }

    // Export private key to PEM format
    char_t *priv_pem_data = NULL;
    size_t priv_pem_size = 0;
    char_t temp_pem_buf[2048];
    size_t temp_pem_size = sizeof(temp_pem_buf);

    if (pemExportEcPrivateKey(SECP384R1_CURVE, &cert_privkey, &cert_pubkey, temp_pem_buf, &temp_pem_size) == NO_ERROR)
    {
        priv_pem_data = osAllocMem(temp_pem_size + 1);
        osMemcpy(priv_pem_data, temp_pem_buf, temp_pem_size);
        priv_pem_data[temp_pem_size] = '\0';
        priv_pem_size = temp_pem_size;
    }

    /* create and sign the certificate */
    X509CertRequestInfo cert_req;
    osMemset(&cert_req, 0x00, sizeof(cert_req));
    cert_req.version = X509_VERSION_1;
    cert_req.subject.name.value = subject;
    cert_req.subject.name.length = osStrlen(subject);

    cert_req.subject.commonName.value = self_sign ? "TeddyCloud Root CA" : subject;
    cert_req.subject.commonName.length = osStrlen(cert_req.subject.commonName.value);
    cert_req.subject.organizationName.value = "Team RevvoX"; //"tonies GmbH";
    cert_req.subject.organizationName.length = 11;
    cert_req.subject.countryName.value = "DE";
    cert_req.subject.countryName.length = 2;
    cert_req.subject.localityName.value = "Duesseldorf";
    cert_req.subject.localityName.length = 11;
    cert_req.subject.stateOrProvinceName.value = "North Rhine-Westphalia";
    cert_req.subject.stateOrProvinceName.length = 22;

    cert_req.subjectPublicKeyInfo.oid.value = EC_PUBLIC_KEY_OID;
    cert_req.subjectPublicKeyInfo.oid.length = sizeof(EC_PUBLIC_KEY_OID);
    cert_req.subjectPublicKeyInfo.ecParams.namedCurve.value = SECP384R1_OID;
    cert_req.subjectPublicKeyInfo.ecParams.namedCurve.length = sizeof(SECP384R1_OID);

    if (self_sign)
    {
        cert_req.attributes.extensionReq.basicConstraints.critical = true;
        cert_req.attributes.extensionReq.basicConstraints.cA = true;
        cert_req.attributes.extensionReq.basicConstraints.pathLenConstraint = -1;

        cert_req.attributes.extensionReq.keyUsage.critical = true;
        cert_req.attributes.extensionReq.keyUsage.bitmap = X509_KEY_USAGE_DIGITAL_SIGNATURE | X509_KEY_USAGE_KEY_CERT_SIGN | X509_KEY_USAGE_CRL_SIGN;
    }
    else
    {
        cert_req.attributes.extensionReq.basicConstraints.critical = true;
        cert_req.attributes.extensionReq.basicConstraints.cA = false;
        cert_req.attributes.extensionReq.basicConstraints.pathLenConstraint = -1;

        if (dns_names_count == 0)
        {
            cert_req.attributes.extensionReq.keyUsage.critical = false;
            cert_req.attributes.extensionReq.keyUsage.bitmap = X509_KEY_USAGE_DIGITAL_SIGNATURE | X509_KEY_USAGE_KEY_AGREEMENT;
            cert_req.attributes.extensionReq.extKeyUsage.critical = false;
            cert_req.attributes.extensionReq.extKeyUsage.bitmap = X509_EXT_KEY_USAGE_CLIENT_AUTH;
        }
        else
        {
            cert_req.attributes.extensionReq.keyUsage.critical = true;
            cert_req.attributes.extensionReq.keyUsage.bitmap = X509_KEY_USAGE_DIGITAL_SIGNATURE | X509_KEY_USAGE_KEY_AGREEMENT;
            cert_req.attributes.extensionReq.extKeyUsage.critical = false;
            cert_req.attributes.extensionReq.extKeyUsage.bitmap = X509_EXT_KEY_USAGE_SERVER_AUTH;

            if (dns_names_count > 0 && dns_names_count <= X509_MAX_SUBJECT_ALT_NAMES)
            {
                cert_req.attributes.extensionReq.subjectAltName.critical = false;
                cert_req.attributes.extensionReq.subjectAltName.numGeneralNames = dns_names_count;
                for (size_t i = 0; i < dns_names_count; i++)
                {
                    cert_req.attributes.extensionReq.subjectAltName.generalNames[i].type = X509_GENERAL_NAME_TYPE_DNS;
                    cert_req.attributes.extensionReq.subjectAltName.generalNames[i].value = dns_names[i];
                    cert_req.attributes.extensionReq.subjectAltName.generalNames[i].length = osStrlen(dns_names[i]);
                }
            }
        }
    }

    X509SerialNumber serial;
    osMemset(&serial, 0x00, sizeof(serial));
    serial.length = serial_number_size;
    serial.value = serial_number;

    X509Validity validity;
    osMemset(&validity, 0x00, sizeof(validity));

    if (self_sign)
    {
        validity.notBefore.year = 2025;
        validity.notBefore.month = 1;
        validity.notBefore.day = 1;
        validity.notBefore.hours = 0;
        validity.notBefore.minutes = 0;
        validity.notBefore.seconds = 0;

        validity.notAfter.year = 2075;
        validity.notAfter.month = 1;
        validity.notAfter.day = 1;
        validity.notAfter.hours = 0;
        validity.notAfter.minutes = 0;
        validity.notAfter.seconds = 0;
    }
    else
    {
        validity.notBefore.year = 2025;
        validity.notBefore.month = 5;
        validity.notBefore.day = 7;
        validity.notBefore.hours = 9;
        validity.notBefore.minutes = 10;
        validity.notBefore.seconds = 18;

        validity.notAfter.year = 2075;
        validity.notAfter.month = 5;
        validity.notAfter.day = 7;
        validity.notAfter.hours = 9;
        validity.notAfter.minutes = 10;
        validity.notAfter.seconds = 18;
    }

    X509SignAlgoId algo;
    osMemset(&algo, 0x00, sizeof(algo));
    algo.oid.value = ECDSA_WITH_SHA384_OID;
    algo.oid.length = sizeof(ECDSA_WITH_SHA384_OID);

    /* create certificate */
    uint8_t *cert_der_data = osAllocMem(8192);
    size_t cert_der_size = 0;
    error_t error = x509CreateCertificate(rand_get_algo(), rand_get_context(), &cert_req, &cert_pubkey, self_sign ? NULL : &issuer_cert, &serial, &validity, &algo, self_sign ? &cert_privkey : &issuer_priv, cert_der_data, &cert_der_size);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("x509CreateCertificate failed: %s\r\n", error2text(error));
        osFreeMem(cert_der_data);
        osFreeMem(priv_data);
        if (priv_pem_data) osFreeMem(priv_pem_data);
        ecFreePublicKey(&cert_pubkey);
        ecFreePrivateKey(&cert_privkey);
        ecFreeDomainParameters(&params);
        if (!self_sign) {
            ecFreePrivateKey(&issuer_priv);
            if (server_ca_der) osFreeMem(server_ca_der);
        }
        return ERROR_FAILURE;
    }

    ecFreePublicKey(&cert_pubkey);
    ecFreePrivateKey(&cert_privkey);
    ecFreeDomainParameters(&params);
    if (!self_sign) {
        ecFreePrivateKey(&issuer_priv);
        if (server_ca_der) osFreeMem(server_ca_der);
    }

    /* export certificate to PEM */
    size_t cert_pem_size = 0;
    if (pemExportCertificate(cert_der_data, cert_der_size, NULL, &cert_pem_size) != NO_ERROR)
    {
        TRACE_ERROR("pemExportCertificate failed\r\n");
        osFreeMem(cert_der_data);
        osFreeMem(priv_data);
        if (priv_pem_data) osFreeMem(priv_pem_data);
        return ERROR_FAILURE;
    }

    char_t *cert_pem_data = osAllocMem(cert_pem_size + 1);
    if (pemExportCertificate(cert_der_data, cert_der_size, cert_pem_data, &cert_pem_size) != NO_ERROR)
    {
        TRACE_ERROR("pemExportCertificate failed\r\n");
        osFreeMem(cert_der_data);
        osFreeMem(priv_data);
        if (priv_pem_data) osFreeMem(priv_pem_data);
        osFreeMem(cert_pem_data);
        return ERROR_FAILURE;
    }

    if (cert_file)
    {
        char *cert_file_full = osAllocMem(256);
        settings_resolve_dir(&cert_file_full, (char *)cert_file, get_settings()->internal.basedirfull);

        FsFile *file = fsOpenFile(cert_file_full, FS_FILE_MODE_WRITE);
        osFreeMem(cert_file_full);
        if (!file)
        {
            TRACE_ERROR("fsOpenFile failed\r\n");
            osFreeMem(cert_der_data);
            osFreeMem(priv_data);
            if (priv_pem_data) osFreeMem(priv_pem_data);
            osFreeMem(cert_pem_data);
            return ERROR_FAILURE;
        }
        if (!cert_der_format)
        {
            fsWriteFile(file, cert_pem_data, cert_pem_size);
        }
        else
        {
            fsWriteFile(file, cert_der_data, cert_der_size);
        }
        fsCloseFile(file);
    }

    if (priv_file)
    {
        char *priv_file_full = osAllocMem(256);
        settings_resolve_dir(&priv_file_full, (char *)priv_file, get_settings()->internal.basedirfull);

        bool priv_pem_format = false;
        size_t priv_file_len = osStrlen(priv_file);
        if (priv_file_len > 4 && (!osStrcasecmp(&priv_file[priv_file_len - 4], ".pem") || !osStrcasecmp(&priv_file[priv_file_len - 4], ".key")))
        {
            priv_pem_format = true;
        }

        FsFile *file = fsOpenFile(priv_file_full, FS_FILE_MODE_WRITE);
        osFreeMem(priv_file_full);
        if (!file)
        {
            TRACE_ERROR("fsOpenFile failed\r\n");
            osFreeMem(cert_der_data);
            osFreeMem(priv_data);
            if (priv_pem_data) osFreeMem(priv_pem_data);
            osFreeMem(cert_pem_data);
            return ERROR_FAILURE;
        }

        if (priv_pem_format)
        {
            if (priv_pem_data)
            {
                fsWriteFile(file, priv_pem_data, priv_pem_size);
            }
            else
            {
                TRACE_ERROR("No PEM private key data available\r\n");
            }
        }
        else
        {
            fsWriteFile(file, priv_data, priv_size);
        }
        fsCloseFile(file);
    }

    osFreeMem(cert_der_data);
    osFreeMem(priv_data);
    if (priv_pem_data) osFreeMem(priv_pem_data);
    osFreeMem(cert_pem_data);

    return NO_ERROR;
}

error_t cert_generate_signed_ec(
    const char *subject,
    const uint8_t *serial_number,
    int serial_number_size,
    bool self_sign,
    bool cert_der_format,
    const char *cert_file,
    const char *priv_file,
    const char *ca_cert_setting,
    const char *ca_key_setting,
    const char *dns_names[],
    size_t dns_names_count)
{
    return cert_generate_signed_ec_internal(
        subject, serial_number, serial_number_size, self_sign, cert_der_format,
        cert_file, priv_file, ca_cert_setting, ca_key_setting, dns_names,
        dns_names_count, NULL, NULL);
}

typedef struct
{
    char *pem;
    size_t pem_size;
    uint8_t *der;
    size_t der_size;
    X509CertInfo info;
} cert_loaded_x509_t;

static void cert_loaded_x509_free(cert_loaded_x509_t *cert)
{
    if (cert == NULL)
        return;
    osFreeMem(cert->pem);
    osFreeMem(cert->der);
    osMemset(cert, 0, sizeof(*cert));
}

static error_t cert_load_x509_file(const char *path, cert_loaded_x509_t *cert)
{
    osMemset(cert, 0, sizeof(*cert));
    error_t error = read_certificate(path, &cert->pem, &cert->pem_size);
    if (error != NO_ERROR)
        return error;

    error = pemImportCertificate(cert->pem, cert->pem_size, NULL,
                                 &cert->der_size, NULL);
    if (error != NO_ERROR)
        goto cleanup;

    cert->der = osAllocMem(cert->der_size);
    if (cert->der == NULL)
    {
        error = ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    error = pemImportCertificate(cert->pem, cert->pem_size, cert->der,
                                 &cert->der_size, NULL);
    if (error == NO_ERROR)
    {
        error = x509ParseCertificateEx(cert->der, cert->der_size,
                                       &cert->info, TRUE);
    }
    if (error == NO_ERROR)
        return NO_ERROR;

cleanup:
    cert_loaded_x509_free(cert);
    return error;
}

static bool_t cert_key_matches(const X509CertInfo *cert, const char *key_path)
{
    char *key_pem = NULL;
    size_t key_size = 0;
    EcPrivateKey private_key;
    EcPublicKey certificate_key;
    EcPublicKey derived_key;
    EcDomainParameters params;
    ecInitPrivateKey(&private_key);
    ecInitPublicKey(&certificate_key);
    ecInitPublicKey(&derived_key);
    ecInitDomainParameters(&params);

    bool_t matches = FALSE;
    if (read_certificate(key_path, &key_pem, &key_size) == NO_ERROR &&
        pemImportEcPrivateKey(key_pem, key_size, NULL, &private_key) == NO_ERROR &&
        x509ImportEcParameters(&cert->tbsCert.subjectPublicKeyInfo.ecParams,
                               &params) == NO_ERROR &&
        x509ImportEcPublicKey(&cert->tbsCert.subjectPublicKeyInfo,
                              &certificate_key) == NO_ERROR &&
        ecGeneratePublicKey(&params, &private_key, &derived_key) == NO_ERROR)
    {
        matches = mpiComp(&certificate_key.q.x, &derived_key.q.x) == 0 &&
                  mpiComp(&certificate_key.q.y, &derived_key.q.y) == 0;
    }

    osFreeMem(key_pem);
    ecFreePrivateKey(&private_key);
    ecFreePublicKey(&certificate_key);
    ecFreePublicKey(&derived_key);
    ecFreeDomainParameters(&params);
    return matches;
}

static bool_t cert_dns_name_matches(const X509GeneralName *name,
                                    const char *expected)
{
    size_t expected_size = osStrlen(expected);
    return name->type == X509_GENERAL_NAME_TYPE_DNS &&
           name->length == expected_size &&
           !osStrncasecmp(name->value, expected, expected_size);
}

static bool_t cert_has_exact_dns_names(const X509CertInfo *cert,
                                       const char *dns_names[],
                                       size_t dns_names_count)
{
    const X509SubjectAltName *san = &cert->tbsCert.extensions.subjectAltName;
    if (san->numGeneralNames != dns_names_count)
        return FALSE;

    for (size_t expected = 0; expected < dns_names_count; expected++)
    {
        bool_t found = FALSE;
        for (size_t actual = 0; actual < san->numGeneralNames; actual++)
        {
            if (cert_dns_name_matches(&san->generalNames[actual], dns_names[expected]))
            {
                found = TRUE;
                break;
            }
        }
        if (!found)
            return FALSE;
    }
    return TRUE;
}

static void cert_sha256_hex(const uint8_t *data, size_t size, char output[65])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256Compute(data, size, digest);
    for (size_t i = 0; i < sizeof(digest); i++)
    {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    output[64] = '\0';
}

static void cert_tb2_set_status(cert_tb2_service_t service, const char *status)
{
    settings_t *settings = get_settings();
    char **target = service == CERT_TB2_SERVICE_HTTPS
                        ? &settings->core.server_cert_tb2_status
                        : &settings->mqtt_server.cert_status;
    char *replacement = strdup(status != NULL ? status : "Unknown");
    if (replacement != NULL)
    {
        osFreeMem(*target);
        *target = replacement;
    }
}

bool_t cert_tb2_hostname_is_valid(const char *hostname, char *message,
                                  size_t message_size)
{
    const char *error = NULL;
    size_t length = hostname != NULL ? osStrlen(hostname) : 0;
    if (length == 0 || length > 253)
        error = "hostname must contain 1 to 253 characters";
    else if (osStrchr(hostname, '.') == NULL)
        error = "hostname must be a fully qualified DNS name";
    else
    {
        size_t label_length = 0;
        bool_t only_digits_and_dots = TRUE;
        for (size_t i = 0; i < length && error == NULL; i++)
        {
            unsigned char c = (unsigned char)hostname[i];
            bool_t alnum = (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9');
            if (c != '.' && (c < '0' || c > '9'))
                only_digits_and_dots = FALSE;
            if (c == '.')
            {
                if (label_length == 0 || hostname[i - 1] == '-')
                    error = "hostname contains an empty or invalid label";
                label_length = 0;
            }
            else if (!alnum && c != '-')
                error = "hostname contains a non-DNS character";
            else
            {
                if (label_length == 0 && c == '-')
                    error = "hostname label starts with a hyphen";
                label_length++;
                if (label_length > 63)
                    error = "hostname label exceeds 63 characters";
            }
        }
        if (error == NULL && (label_length == 0 || hostname[length - 1] == '-'))
            error = "hostname contains an empty or invalid label";
        if (error == NULL && only_digits_and_dots)
            error = "IP literals are not accepted";
    }

    if (message != NULL && message_size > 0)
        osSnprintf(message, message_size, "%s", error != NULL ? error : "valid");
    return error == NULL;
}

static size_t cert_tb2_dns_names(cert_tb2_service_t service, const char *hostname,
                                 const char *dns_names[], size_t capacity)
{
    static const char *https_defaults[] = {"tbs2.tonie.cloud"};
    static const char *mqtt_defaults[] = {
        "ici.tonie.cloud", "ici.dev.tonie.cloud", "ici.stage.tonie.cloud"};
    const char **defaults = service == CERT_TB2_SERVICE_HTTPS
                                ? https_defaults
                                : mqtt_defaults;
    size_t defaults_count = service == CERT_TB2_SERVICE_HTTPS ? 1 : 3;
    size_t count = 0;
    dns_names[count++] = hostname;
    for (size_t i = 0; i < defaults_count && count < capacity; i++)
    {
        bool_t duplicate = FALSE;
        for (size_t j = 0; j < count; j++)
        {
            if (!osStrcasecmp(dns_names[j], defaults[i]))
                duplicate = TRUE;
        }
        if (!duplicate)
            dns_names[count++] = defaults[i];
    }
    return count;
}

static void cert_tb2_resolve_path(const char *setting, char path[TB2_CERT_PATH_SIZE])
{
    char *path_ptr = path;
    path[0] = '\0';
    settings_resolve_dir(&path_ptr, (char *)settings_get_string(setting),
                         get_settings()->internal.basedirfull);
}

static error_t cert_tb2_append(char *output, size_t output_size,
                               const char *base, const char *suffix)
{
    size_t base_size = osStrlen(base);
    size_t suffix_size = osStrlen(suffix);
    if (base_size + suffix_size >= output_size)
        return ERROR_INVALID_LENGTH;
    osMemcpy(output, base, base_size);
    osMemcpy(output + base_size, suffix, suffix_size + 1);
    return NO_ERROR;
}

static error_t cert_tb2_join(char *output, size_t output_size,
                             const char *directory, const char *filename)
{
    size_t directory_size = osStrlen(directory);
    size_t filename_size = osStrlen(filename);
    if (directory_size + 1 + filename_size >= output_size)
        return ERROR_INVALID_LENGTH;
    osMemcpy(output, directory, directory_size);
    output[directory_size] = PATH_SEPARATOR;
    osMemcpy(output + directory_size + 1, filename, filename_size + 1);
    return NO_ERROR;
}

static error_t cert_tb2_validate_pair(const char *cert_path, const char *key_path,
                                      const char *ca_cert_path, const char *ca_key_path,
                                      const char *dns_names[], size_t dns_names_count,
                                      char fingerprint[65], char *message,
                                      size_t message_size)
{
    cert_loaded_x509_t leaf;
    cert_loaded_x509_t ca;
    error_t error = cert_load_x509_file(cert_path, &leaf);
    if (error != NO_ERROR)
    {
        osSnprintf(message, message_size, "certificate is missing or unreadable");
        return error;
    }
    error = cert_load_x509_file(ca_cert_path, &ca);
    if (error != NO_ERROR)
    {
        osSnprintf(message, message_size, "TB2 CA certificate is unreadable");
        cert_loaded_x509_free(&leaf);
        return error;
    }

    cert_sha256_hex(leaf.der, leaf.der_size, fingerprint);

    if (!cert_key_matches(&ca.info, ca_key_path))
    {
        osSnprintf(message, message_size, "TB2 CA certificate and key do not match");
        error = ERROR_FAILURE;
    }
    else if (!cert_key_matches(&leaf.info, key_path))
    {
        osSnprintf(message, message_size, "server certificate and key do not match");
        error = ERROR_FAILURE;
    }
    else if (x509ValidateCertificate(&leaf.info, &ca.info, 0) != NO_ERROR)
    {
        osSnprintf(message, message_size, "server certificate is not valid under the current TB2 CA");
        error = ERROR_FAILURE;
    }
    else if (leaf.info.tbsCert.extensions.basicConstraints.cA)
    {
        osSnprintf(message, message_size, "server certificate is marked as a CA");
        error = ERROR_FAILURE;
    }
    else if ((leaf.info.tbsCert.extensions.extKeyUsage.bitmap &
              X509_EXT_KEY_USAGE_SERVER_AUTH) == 0)
    {
        osSnprintf(message, message_size, "server certificate lacks serverAuth");
        error = ERROR_FAILURE;
    }
    else if (!cert_has_exact_dns_names(&leaf.info, dns_names, dns_names_count))
    {
        osSnprintf(message, message_size, "server certificate SANs do not match the configured hostname");
        error = ERROR_FAILURE;
    }
    else
    {
        time_t now = getCurrentUnixTime();
        time_t not_after = convertDateToUnixTime(&leaf.info.tbsCert.validity.notAfter);
        if (now != 0 && not_after <= now + TB2_CERT_NEAR_EXPIRY_SECONDS)
        {
            osSnprintf(message, message_size, "server certificate is expired or near expiry");
            error = ERROR_FAILURE;
        }
        else
        {
            osSnprintf(message, message_size, "certificate matches");
            error = NO_ERROR;
        }
    }

    cert_loaded_x509_free(&leaf);
    cert_loaded_x509_free(&ca);
    return error;
}

error_t cert_validate_server_files(const char *ca_cert_path,
                                   const char *ca_der_path,
                                   const char *ca_key_path,
                                   const char *cert_path,
                                   const char *key_path,
                                   const char *dns_names[],
                                   size_t dns_names_count,
                                   bool_t require_server_auth,
                                   char *message,
                                   size_t message_size)
{
    cert_loaded_x509_t ca;
    cert_loaded_x509_t ca_der;
    cert_loaded_x509_t leaf;
    error_t error = cert_load_x509_file(ca_cert_path, &ca);
    if (error != NO_ERROR)
    {
        osSnprintf(message, message_size, "CA certificate is missing or unreadable");
        return error;
    }
    error = cert_load_x509_file(ca_der_path, &ca_der);
    if (error != NO_ERROR)
    {
        osSnprintf(message, message_size, "CA DER certificate is missing or unreadable");
        cert_loaded_x509_free(&ca);
        return error;
    }
    error = cert_load_x509_file(cert_path, &leaf);
    if (error != NO_ERROR)
    {
        osSnprintf(message, message_size, "server certificate is missing or unreadable");
        cert_loaded_x509_free(&ca_der);
        cert_loaded_x509_free(&ca);
        return error;
    }

    if (ca.der_size != ca_der.der_size ||
        osMemcmp(ca.der, ca_der.der, ca.der_size) != 0)
    {
        osSnprintf(message, message_size, "CA PEM and DER certificates differ");
        error = ERROR_FAILURE;
    }
    else if (!ca.info.tbsCert.extensions.basicConstraints.cA)
    {
        osSnprintf(message, message_size, "CA certificate is not marked as a CA");
        error = ERROR_FAILURE;
    }
    else if (!cert_key_matches(&ca.info, ca_key_path))
    {
        osSnprintf(message, message_size, "CA certificate and private key do not match");
        error = ERROR_FAILURE;
    }
    else if (!cert_key_matches(&leaf.info, key_path))
    {
        osSnprintf(message, message_size, "server certificate and private key do not match");
        error = ERROR_FAILURE;
    }
    else if (x509ValidateCertificate(&leaf.info, &ca.info, 0) != NO_ERROR)
    {
        osSnprintf(message, message_size, "server certificate is not signed by the configured CA");
        error = ERROR_FAILURE;
    }
    else if (leaf.info.tbsCert.extensions.basicConstraints.cA)
    {
        osSnprintf(message, message_size, "server certificate is marked as a CA");
        error = ERROR_FAILURE;
    }
    else if (require_server_auth &&
             (leaf.info.tbsCert.extensions.extKeyUsage.bitmap &
              X509_EXT_KEY_USAGE_SERVER_AUTH) == 0)
    {
        osSnprintf(message, message_size, "server certificate lacks serverAuth");
        error = ERROR_FAILURE;
    }
    else if (dns_names_count > 0 &&
             !cert_has_exact_dns_names(&leaf.info, dns_names, dns_names_count))
    {
        osSnprintf(message, message_size, "server certificate SANs do not match");
        error = ERROR_FAILURE;
    }
    else
    {
        osSnprintf(message, message_size, "certificate set is valid");
        error = NO_ERROR;
    }

    cert_loaded_x509_free(&leaf);
    cert_loaded_x509_free(&ca_der);
    cert_loaded_x509_free(&ca);
    return error;
}

error_t cert_validate_client_files(const char *ca_cert_path,
                                   const char *cert_path,
                                   const char *key_path,
                                   char *message,
                                   size_t message_size)
{
    cert_loaded_x509_t ca;
    cert_loaded_x509_t leaf;
    error_t error = cert_load_x509_file(ca_cert_path, &ca);
    if (error != NO_ERROR)
    {
        osSnprintf(message, message_size, "client CA is missing or unreadable");
        return error;
    }
    error = cert_load_x509_file(cert_path, &leaf);
    if (error != NO_ERROR)
    {
        osSnprintf(message, message_size, "client certificate is missing or unreadable");
        cert_loaded_x509_free(&ca);
        return error;
    }

    if (!cert_key_matches(&leaf.info, key_path))
    {
        osSnprintf(message, message_size, "client certificate and private key do not match");
        error = ERROR_FAILURE;
    }
    else if (x509ValidateCertificate(&leaf.info, &ca.info, 0) != NO_ERROR)
    {
        osSnprintf(message, message_size, "client certificate is not signed by its CA");
        error = ERROR_FAILURE;
    }
    else if (leaf.info.tbsCert.extensions.basicConstraints.cA)
    {
        osSnprintf(message, message_size, "client certificate is marked as a CA");
        error = ERROR_FAILURE;
    }
    else
    {
        osSnprintf(message, message_size, "client certificate set is valid");
        error = NO_ERROR;
    }
    cert_loaded_x509_free(&leaf);
    cert_loaded_x509_free(&ca);
    return error;
}

static error_t cert_tb2_write_archive(cert_tb2_service_t service,
                                      const char *cert_path, const char *key_path,
                                      const char *reason, const char *old_hostname,
                                      const char *new_hostname,
                                      const char *old_fingerprint,
                                      const char *new_fingerprint,
                                      const char *ca_fingerprint)
{
    if (!fsFileExists(cert_path) && !fsFileExists(key_path))
        return NO_ERROR;

    DateTime now;
    getCurrentDate(&now);
    char archive_path[TB2_CERT_PATH_SIZE];
    const char *service_name = service == CERT_TB2_SERVICE_HTTPS ? "https" : "mqtt";
    for (unsigned int suffix = 0; suffix < 100; suffix++)
    {
        char suffix_text[8] = "";
        if (suffix > 0)
            osSnprintf(suffix_text, sizeof(suffix_text), "-%u", suffix);
        osSnprintf(archive_path, sizeof(archive_path),
                   "%s%ccerts%carchive%ctb2%c%04u%02u%02uT%02u%02u%02uZ%s%c%s",
                   get_settings()->internal.basedirfull, PATH_SEPARATOR,
                   PATH_SEPARATOR, PATH_SEPARATOR, PATH_SEPARATOR,
                   now.year, now.month, now.day, now.hours, now.minutes, now.seconds,
                   suffix_text, PATH_SEPARATOR, service_name);
        if (!fsDirExists(archive_path))
            break;
    }

    error_t error = fsCreateDirEx(archive_path, TRUE);
    if (error != NO_ERROR && !fsDirExists(archive_path))
        return error;

    char target[TB2_CERT_PATH_SIZE];
    if (fsFileExists(cert_path))
    {
        error = cert_tb2_join(target, sizeof(target), archive_path, "server.pem");
        if (error == NO_ERROR)
            error = fsCopyFile(cert_path, target, FALSE);
        if (error != NO_ERROR)
            return error;
    }
    if (fsFileExists(key_path))
    {
        error = cert_tb2_join(target, sizeof(target), archive_path, "server.key");
        if (error == NO_ERROR)
            error = fsCopyFile(key_path, target, FALSE);
        if (error != NO_ERROR)
            return error;
    }

    error = cert_tb2_join(target, sizeof(target), archive_path, "metadata.json");
    if (error != NO_ERROR)
        return error;
    char *metadata = custom_asprintf(
        "{\"service\":\"%s\",\"reason\":\"%s\",\"oldHostname\":\"%s\","
        "\"newHostname\":\"%s\",\"oldFingerprint\":\"%s\","
        "\"newFingerprint\":\"%s\",\"caFingerprint\":\"%s\","
        "\"verification\":\"passed\"}\n",
        service_name, reason, old_hostname, new_hostname,
        old_fingerprint, new_fingerprint, ca_fingerprint);
    if (metadata == NULL)
        return ERROR_OUT_OF_MEMORY;
    FsFile *file = fsOpenFile(target, FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE | FS_FILE_MODE_TRUNC);
    if (file == NULL)
    {
        osFreeMem(metadata);
        return ERROR_OPEN_FAILED;
    }
    error = fsWriteFile(file, metadata, osStrlen(metadata));
    fsCloseFile(file);
    osFreeMem(metadata);
    return error;
}

static error_t cert_tb2_reload(cert_tb2_service_t service)
{
    return service == CERT_TB2_SERVICE_HTTPS
               ? settings_reload_tb2_server_certificate()
               : mqtt_server_reload_certificate();
}

error_t cert_tb2_reconcile_service(cert_tb2_service_t service,
                                   const char *reason,
                                   const char *old_hostname,
                                   cert_tb2_reconcile_result_t *result)
{
    cert_tb2_reconcile_result_t local_result;
    if (result == NULL)
        result = &local_result;
    osMemset(result, 0, sizeof(*result));

    settings_t *settings = get_settings();
    const char *hostname = service == CERT_TB2_SERVICE_HTTPS
                               ? settings->core.server_cert_tb2_hostname
                               : settings->mqtt_server.hostname;
    char validation[192];
    if (!cert_tb2_hostname_is_valid(hostname, validation, sizeof(validation)))
    {
        cert_tb2_set_status(service, validation);
        osSnprintf(result->message, sizeof(result->message), "%s", validation);
        return ERROR_INVALID_PARAMETER;
    }

    const char *dns_names[X509_MAX_SUBJECT_ALT_NAMES];
    size_t dns_names_count = cert_tb2_dns_names(service, hostname, dns_names,
                                                X509_MAX_SUBJECT_ALT_NAMES);
    const char *cert_setting = service == CERT_TB2_SERVICE_HTTPS
                                   ? "core.server_cert_tb2.file.crt"
                                   : "mqtt_server.cert.crt";
    const char *key_setting = service == CERT_TB2_SERVICE_HTTPS
                                  ? "core.server_cert_tb2.file.key"
                                  : "mqtt_server.cert.key";
    char cert_path[TB2_CERT_PATH_SIZE];
    char key_path[TB2_CERT_PATH_SIZE];
    char ca_cert_path[TB2_CERT_PATH_SIZE];
    char ca_key_path[TB2_CERT_PATH_SIZE];
    cert_tb2_resolve_path(cert_setting, cert_path);
    cert_tb2_resolve_path(key_setting, key_path);
    cert_tb2_resolve_path("core.server_cert_tb2.file.ca", ca_cert_path);
    cert_tb2_resolve_path("core.server_cert_tb2.file.ca_key", ca_key_path);

    cert_tb2_rotation_lock();
    char old_fingerprint[65] = "unavailable";
    error_t error = cert_tb2_validate_pair(cert_path, key_path, ca_cert_path,
                                            ca_key_path, dns_names, dns_names_count,
                                            old_fingerprint, validation,
                                            sizeof(validation));
    if (error == NO_ERROR)
    {
        osSnprintf(result->message, sizeof(result->message),
                   "Certificate already matches; no files changed");
        cert_tb2_set_status(service, result->message);
        cert_tb2_rotation_unlock();
        return NO_ERROR;
    }

    char temp_cert[TB2_CERT_PATH_SIZE] = {0};
    char temp_key[TB2_CERT_PATH_SIZE] = {0};
    char rollback_cert[TB2_CERT_PATH_SIZE] = {0};
    char rollback_key[TB2_CERT_PATH_SIZE] = {0};
    error = cert_tb2_append(temp_cert, sizeof(temp_cert), cert_path, ".new");
    if (error == NO_ERROR)
        error = cert_tb2_append(temp_key, sizeof(temp_key), key_path, ".new");
    if (error == NO_ERROR)
        error = cert_tb2_append(rollback_cert, sizeof(rollback_cert), cert_path, ".rollback");
    if (error == NO_ERROR)
        error = cert_tb2_append(rollback_key, sizeof(rollback_key), key_path, ".rollback");
    if (error != NO_ERROR)
        goto failed;
    fsDeleteFile(temp_cert);
    fsDeleteFile(temp_key);
    fsDeleteFile(rollback_cert);
    fsDeleteFile(rollback_key);

    char parent[TB2_CERT_PATH_SIZE];
    osSnprintf(parent, sizeof(parent), "%s", cert_path);
    fsRemoveFilename(parent);
    error = fsCreateDirEx(parent, TRUE);
    if (error != NO_ERROR && !fsDirExists(parent))
        goto failed;
    osSnprintf(parent, sizeof(parent), "%s", key_path);
    fsRemoveFilename(parent);
    error = fsCreateDirEx(parent, TRUE);
    if (error != NO_ERROR && !fsDirExists(parent))
        goto failed;

    uint8_t serial[14];
    size_t serial_length = sizeof(serial);
    cert_generate_serial(serial, &serial_length);
    error = cert_generate_signed_ec(hostname, serial, serial_length, FALSE, FALSE,
                                    temp_cert, temp_key,
                                    "internal.server_tb2.ca",
                                    "internal.server_tb2.ca_key",
                                    dns_names, dns_names_count);
    if (error != NO_ERROR)
        goto failed;

    char new_fingerprint[65] = "unavailable";
    error = cert_tb2_validate_pair(temp_cert, temp_key, ca_cert_path, ca_key_path,
                                   dns_names, dns_names_count, new_fingerprint,
                                   validation, sizeof(validation));
    if (error != NO_ERROR)
        goto failed;

    cert_loaded_x509_t ca;
    char ca_fingerprint[65] = "unavailable";
    if (cert_load_x509_file(ca_cert_path, &ca) == NO_ERROR)
    {
        cert_sha256_hex(ca.der, ca.der_size, ca_fingerprint);
        cert_loaded_x509_free(&ca);
    }
    error = cert_tb2_write_archive(service, cert_path, key_path,
                                   reason != NULL ? reason : "certificate mismatch",
                                   old_hostname != NULL ? old_hostname : hostname,
                                   hostname, old_fingerprint, new_fingerprint,
                                   ca_fingerprint);
    if (error != NO_ERROR)
        goto failed;
    result->archived = fsFileExists(cert_path) || fsFileExists(key_path);

    bool_t had_cert = fsFileExists(cert_path);
    bool_t had_key = fsFileExists(key_path);
    if (had_cert && fsCopyFile(cert_path, rollback_cert, TRUE) != NO_ERROR)
    {
        error = ERROR_WRITE_FAILED;
        goto failed;
    }
    if (had_key && fsCopyFile(key_path, rollback_key, TRUE) != NO_ERROR)
    {
        error = ERROR_WRITE_FAILED;
        goto failed;
    }
    error = fsMoveFile(temp_cert, cert_path, TRUE);
    if (error == NO_ERROR)
        error = fsMoveFile(temp_key, key_path, TRUE);
    if (error == NO_ERROR)
        error = cert_tb2_reload(service);
    if (error != NO_ERROR)
    {
        if (had_cert)
            fsMoveFile(rollback_cert, cert_path, TRUE);
        else
            fsDeleteFile(cert_path);
        if (had_key)
            fsMoveFile(rollback_key, key_path, TRUE);
        else
            fsDeleteFile(key_path);
        cert_tb2_reload(service);
        goto failed;
    }

    fsDeleteFile(rollback_cert);
    fsDeleteFile(rollback_key);
    result->rotated = TRUE;
    result->tls_reloaded = TRUE;
    result->restart_required = FALSE;
    osSnprintf(result->message, sizeof(result->message),
               "Rebuilt and reloaded certificate for %s", hostname);
    cert_tb2_set_status(service, result->message);
    TRACE_INFO("Rebuilt TB2 %s server certificate hostname=%s reason=%s restart_required=false\r\n",
               service == CERT_TB2_SERVICE_HTTPS ? "HTTPS" : "MQTT",
               hostname, reason != NULL ? reason : "certificate mismatch");
    cert_tb2_rotation_unlock();
    return NO_ERROR;

failed:
    fsDeleteFile(temp_cert);
    fsDeleteFile(temp_key);
    fsDeleteFile(rollback_cert);
    fsDeleteFile(rollback_key);
    osSnprintf(result->message, sizeof(result->message),
               "Certificate rebuild failed; active files kept or restored");
    cert_tb2_set_status(service, result->message);
    TRACE_ERROR("TB2 %s certificate rebuild failed hostname=%s error=%s\r\n",
                service == CERT_TB2_SERVICE_HTTPS ? "HTTPS" : "MQTT",
                hostname, error2text(error));
    cert_tb2_rotation_unlock();
    return error != NO_ERROR ? error : ERROR_FAILURE;
}

error_t cert_tb2_reconcile_all(const char *reason)
{
    error_t https_error = cert_tb2_reconcile_service(CERT_TB2_SERVICE_HTTPS,
                                                      reason, NULL, NULL);
    error_t mqtt_error = cert_tb2_reconcile_service(CERT_TB2_SERVICE_MQTT,
                                                     reason, NULL, NULL);
    return https_error != NO_ERROR ? https_error : mqtt_error;
}

error_t cert_generate_default_tb2()
{
    char *ca = cert_resolve_configured_path("core.server_cert_tb2.file.ca");
    char *caDer = cert_resolve_configured_path("core.server_cert_tb2.file.ca_der");
    char *caKey = cert_resolve_configured_path("core.server_cert_tb2.file.ca_key");
    char *httpsCert = cert_resolve_configured_path("core.server_cert_tb2.file.crt");
    char *httpsKey = cert_resolve_configured_path("core.server_cert_tb2.file.key");
    char *mqttCert = cert_resolve_configured_path("mqtt_server.cert.crt");
    char *mqttKey = cert_resolve_configured_path("mqtt_server.cert.key");
    if (ca == NULL || caDer == NULL || caKey == NULL || httpsCert == NULL ||
        httpsKey == NULL || mqttCert == NULL || mqttKey == NULL)
    {
        osFreeMem(ca); osFreeMem(caDer); osFreeMem(caKey);
        osFreeMem(httpsCert); osFreeMem(httpsKey);
        osFreeMem(mqttCert); osFreeMem(mqttKey);
        return ERROR_OUT_OF_MEMORY;
    }

    bool_t caExists = cert_path_is_nonempty(ca);
    bool_t caKeyExists = cert_path_is_nonempty(caKey);
    bool_t caDerExists = cert_path_is_nonempty(caDer);
    bool_t httpsCertExists = cert_path_is_nonempty(httpsCert);
    bool_t httpsKeyExists = cert_path_is_nonempty(httpsKey);
    bool_t mqttCertExists = cert_path_is_nonempty(mqttCert);
    bool_t mqttKeyExists = cert_path_is_nonempty(mqttKey);
    TRACE_INFO("TB2 certificate initialization paths ca='%s' ca_der='%s' ca_key='%s' https_cert='%s' https_key='%s' mqtt_cert='%s' mqtt_key='%s'\r\n",
               ca, caDer, caKey, httpsCert, httpsKey, mqttCert, mqttKey);

    error_t error = NO_ERROR;
    if (caExists != caKeyExists || httpsCertExists != httpsKeyExists ||
        mqttCertExists != mqttKeyExists)
    {
        TRACE_ERROR("TB2 certificate set contains an incomplete certificate/key pair; no files changed\r\n");
        error = ERROR_ABORTED;
        goto cleanup;
    }
    if (!caExists && (caDerExists || httpsCertExists || mqttCertExists))
    {
        TRACE_ERROR("TB2 certificate set contains dependent files without a complete CA pair; no files changed\r\n");
        error = ERROR_ABORTED;
        goto cleanup;
    }

    char *caNew = custom_asprintf("%s.new", ca);
    char *caDerNew = custom_asprintf("%s.new", caDer);
    char *caKeyNew = custom_asprintf("%s.new", caKey);
    char *httpsCertNew = custom_asprintf("%s.new", httpsCert);
    char *httpsKeyNew = custom_asprintf("%s.new", httpsKey);
    char *mqttCertNew = custom_asprintf("%s.new", mqttCert);
    char *mqttKeyNew = custom_asprintf("%s.new", mqttKey);
    if (caNew == NULL || caDerNew == NULL || caKeyNew == NULL ||
        httpsCertNew == NULL || httpsKeyNew == NULL ||
        mqttCertNew == NULL || mqttKeyNew == NULL)
    {
        error = ERROR_OUT_OF_MEMORY;
        goto staged_cleanup;
    }
    fsDeleteFile(caNew); fsDeleteFile(caDerNew); fsDeleteFile(caKeyNew);
    fsDeleteFile(httpsCertNew); fsDeleteFile(httpsKeyNew);
    fsDeleteFile(mqttCertNew); fsDeleteFile(mqttKeyNew);
    error = cert_prepare_parent(ca);
    if (error != NO_ERROR)
        goto staged_cleanup;

    uint8_t serial[14];
    size_t serialLength = sizeof(serial);
    if (!caExists)
    {
        cert_generate_serial(serial, &serialLength);
        error = cert_generate_signed_ec("TeddyCloud Root CA", serial,
                                        serialLength, true, false,
                                        caNew, caKeyNew, NULL, NULL, NULL, 0);
        if (error != NO_ERROR)
            goto staged_cleanup;
    }

    const char *effectiveCa = caExists ? ca : caNew;
    const char *effectiveCaKey = caKeyExists ? caKey : caKeyNew;
    if (!caDerExists)
    {
        char *caData = NULL;
        size_t caDataLength = 0;
        error = read_certificate(effectiveCa, &caData, &caDataLength);
        if (error == NO_ERROR)
            error = convert_PEM_to_DER(caData, caDerNew);
        osFreeMem(caData);
        if (error != NO_ERROR)
            goto staged_cleanup;
    }

    const char *httpsNames[X509_MAX_SUBJECT_ALT_NAMES];
    size_t httpsNameCount = cert_tb2_dns_names(
        CERT_TB2_SERVICE_HTTPS, get_settings()->core.server_cert_tb2_hostname,
        httpsNames, X509_MAX_SUBJECT_ALT_NAMES);
    const char *mqttNames[X509_MAX_SUBJECT_ALT_NAMES];
    size_t mqttNameCount = cert_tb2_dns_names(
        CERT_TB2_SERVICE_MQTT, get_settings()->mqtt_server.hostname,
        mqttNames, X509_MAX_SUBJECT_ALT_NAMES);
    if (!httpsCertExists)
    {
        serialLength = sizeof(serial);
        cert_generate_serial(serial, &serialLength);
        error = cert_generate_signed_ec_internal(
            get_settings()->core.server_cert_tb2_hostname, serial, serialLength,
            false, false, httpsCertNew, httpsKeyNew, NULL, NULL,
            httpsNames, httpsNameCount, effectiveCa, effectiveCaKey);
        if (error != NO_ERROR)
            goto staged_cleanup;
    }
    if (!mqttCertExists)
    {
        serialLength = sizeof(serial);
        cert_generate_serial(serial, &serialLength);
        error = cert_generate_signed_ec_internal(
            get_settings()->mqtt_server.hostname, serial, serialLength,
            false, false, mqttCertNew, mqttKeyNew, NULL, NULL,
            mqttNames, mqttNameCount, effectiveCa, effectiveCaKey);
        if (error != NO_ERROR)
            goto staged_cleanup;
    }

    const char *effectiveCaDer = caDerExists ? caDer : caDerNew;
    const char *effectiveHttpsCert = httpsCertExists ? httpsCert : httpsCertNew;
    const char *effectiveHttpsKey = httpsKeyExists ? httpsKey : httpsKeyNew;
    const char *effectiveMqttCert = mqttCertExists ? mqttCert : mqttCertNew;
    const char *effectiveMqttKey = mqttKeyExists ? mqttKey : mqttKeyNew;
    char validation[192];
    error = cert_validate_server_files(effectiveCa, effectiveCaDer, effectiveCaKey,
                                       effectiveHttpsCert, effectiveHttpsKey,
                                       httpsNames, httpsNameCount, TRUE,
                                       validation, sizeof(validation));
    if (error == NO_ERROR)
    {
        error = cert_validate_server_files(effectiveCa, effectiveCaDer,
                                           effectiveCaKey, effectiveMqttCert,
                                           effectiveMqttKey, mqttNames,
                                           mqttNameCount, TRUE, validation,
                                           sizeof(validation));
    }
    if (error != NO_ERROR)
    {
        TRACE_ERROR("TB2 certificate staging validation failed: %s; no files changed\r\n",
                    validation);
        goto staged_cleanup;
    }

    bool_t publishedCa = FALSE, publishedCaDer = FALSE, publishedCaKey = FALSE;
    bool_t publishedHttpsCert = FALSE, publishedHttpsKey = FALSE;
    bool_t publishedMqttCert = FALSE, publishedMqttKey = FALSE;
    if (!caExists) error = cert_publish_new_file(caNew, ca, &publishedCa);
    if (error == NO_ERROR && !caKeyExists)
        error = cert_publish_new_file(caKeyNew, caKey, &publishedCaKey);
    if (error == NO_ERROR && !caDerExists)
        error = cert_publish_new_file(caDerNew, caDer, &publishedCaDer);
    if (error == NO_ERROR && !httpsCertExists)
        error = cert_publish_new_file(httpsCertNew, httpsCert, &publishedHttpsCert);
    if (error == NO_ERROR && !httpsKeyExists)
        error = cert_publish_new_file(httpsKeyNew, httpsKey, &publishedHttpsKey);
    if (error == NO_ERROR && !mqttCertExists)
        error = cert_publish_new_file(mqttCertNew, mqttCert, &publishedMqttCert);
    if (error == NO_ERROR && !mqttKeyExists)
        error = cert_publish_new_file(mqttKeyNew, mqttKey, &publishedMqttKey);
    if (error != NO_ERROR)
    {
        if (publishedMqttKey) fsDeleteFile(mqttKey);
        if (publishedMqttCert) fsDeleteFile(mqttCert);
        if (publishedHttpsKey) fsDeleteFile(httpsKey);
        if (publishedHttpsCert) fsDeleteFile(httpsCert);
        if (publishedCaDer) fsDeleteFile(caDer);
        if (publishedCaKey) fsDeleteFile(caKey);
        if (publishedCa) fsDeleteFile(ca);
        TRACE_ERROR("TB2 certificate publication failed and newly published files were rolled back: %s\r\n",
                    error2text(error));
        goto staged_cleanup;
    }

    TRACE_INFO("TB2 certificate set is complete and valid; existing files were preserved\r\n");
    error = settings_try_load_certs_id(0);

staged_cleanup:
    if (caNew != NULL) fsDeleteFile(caNew);
    if (caDerNew != NULL) fsDeleteFile(caDerNew);
    if (caKeyNew != NULL) fsDeleteFile(caKeyNew);
    if (httpsCertNew != NULL) fsDeleteFile(httpsCertNew);
    if (httpsKeyNew != NULL) fsDeleteFile(httpsKeyNew);
    if (mqttCertNew != NULL) fsDeleteFile(mqttCertNew);
    if (mqttKeyNew != NULL) fsDeleteFile(mqttKeyNew);
    osFreeMem(caNew); osFreeMem(caDerNew); osFreeMem(caKeyNew);
    osFreeMem(httpsCertNew); osFreeMem(httpsKeyNew);
    osFreeMem(mqttCertNew); osFreeMem(mqttKeyNew);

cleanup:
    osFreeMem(ca); osFreeMem(caDer); osFreeMem(caKey);
    osFreeMem(httpsCert); osFreeMem(httpsKey);
    osFreeMem(mqttCert); osFreeMem(mqttKey);
    return error;
}

error_t cert_generate_mac_tb2(const char *mac, const char *dest, bool add_to_settings)
{
    if (!mac || osStrlen(mac) != 12)
    {
        return ERROR_FAILURE;
    }

    char mac_upper[13];
    osStrncpy(mac_upper, mac, 13);
    mac_upper[12] = '\0';
    osStringToUpper(mac_upper);

    char mac_lower[13];
    osStrncpy(mac_lower, mac, 13);
    mac_lower[12] = '\0';
    osStringToLower(mac_lower);

    char *actual_dest = NULL;
    if (!dest) {
        char *certdir = get_settings()->internal.certdirfull_tb2;
        if (!certdir) certdir = "certs/client_tb2";
        actual_dest = custom_asprintf("%s/%s", certdir, mac_lower);
    } else {
        actual_dest = custom_asprintf("%s", dest);
    }

    char_t *client_der_file = custom_asprintf("%s/client.fake.der", actual_dest);
    char_t *private_der_file = custom_asprintf("%s/private.fake.der", actual_dest);
    char_t *ca_der_file = custom_asprintf("%s/ca.fake.der", actual_dest);

    if (fsFileExists(client_der_file) && fsFileExists(private_der_file) && fsFileExists(ca_der_file))
    {
        TRACE_INFO("Fake TB2 client certificates already exist for MAC %s in %s, skipping generation.\r\n", mac_upper, actual_dest);
    }
    else
    {
        if (!fsDirExists(actual_dest)) {
            fsCreateDirEx(actual_dest, true);
        }

        uint8_t serial[7];
        size_t serial_length = 7;
        char_t subj[32];

        serial[0] = 0;
        hex_string_to_bytes(mac_upper, &serial[1]);
        cert_truncate_serial(serial, &serial_length);

        osSprintf(subj, "%s", mac_upper);

        if (cert_generate_signed_ec(subj, serial, 7, false, true, client_der_file, private_der_file, "internal.server_tb2.ca", "internal.server_tb2.ca_key", NULL, 0) != NO_ERROR)
        {
            TRACE_ERROR("cert_generate_signed_ec (DER) failed for client cert\r\n");
            osFreeMem(client_der_file);
            osFreeMem(private_der_file);
            osFreeMem(ca_der_file);
            osFreeMem(actual_dest);
            return ERROR_FAILURE;
        }

        /* Copy the TB2 ca.der into the dir as ca.fake.der */
        char *server_ca_der = osAllocMem(256);
        settings_resolve_dir(&server_ca_der, (char*)settings_get_string("core.server_cert_tb2.file.ca_der"), get_settings()->internal.basedirfull);
        
        if (fsCopyFile(server_ca_der, ca_der_file, true) != NO_ERROR) {
            TRACE_ERROR("Failed to copy TB2 ca.der to %s\r\n", ca_der_file);
        }
        osFreeMem(server_ca_der);

        TRACE_INFO("Generated fake TB2 client certificates for MAC %s in %s\r\n", mac_upper, actual_dest);
    }

    /* Update the overlay for the MAC if appropriate */
    if (add_to_settings)
    {
        settings_t *overlay_settings = get_settings_cn(mac_upper);
        if (overlay_settings)
        {
            uint8_t overlayId = overlay_settings->internal.overlayNumber;
            settings_set_unsigned_id("toniebox.boxGeneration", 2, overlayId);
            settings_set_string_id("core.client_cert_fake.file.ca", ca_der_file, overlayId);
            settings_set_string_id("core.client_cert_fake.file.crt", client_der_file, overlayId);
            settings_set_string_id("core.client_cert_fake.file.key", private_der_file, overlayId);
            settings_save();
            TRACE_INFO("Added configuration overlay for %s\r\n", mac_upper);
        }
    }

    osFreeMem(ca_der_file);
    osFreeMem(client_der_file);
    osFreeMem(private_der_file);
    osFreeMem(actual_dest);

    return NO_ERROR;
}
