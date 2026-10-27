/*
 * Android backend for UxPlay's dnssd API, announcing with UxPlay's own mDNS responder
 * (lib/mdnsd) rather than Android's NsdManager.
 *
 * NsdManager's announcements never reached Macs on some home routers, which drop multicast
 * between Wi-Fi clients. lib/mdnsd, built with MDNSD_ALWAYS_UNICAST_REPLY, also answers each
 * query directly to the device that asked, which those routers pass.
 *
 * Differs from lib/mdnsd/dnssd_mdnsd.c in the host name: Android's gethostname() returns
 * "localhost", so the host is named after the receiver instead.
 *
 * GPL-3.0, derived from UxPlay (lib/mdnsd/dnssd_mdnsd.c).
 */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnssd.h"
#include "dnssdint.h"
#include "utils.h"
#include "mdnsd/mdnsd.h"

typedef struct dnssd_private_s {
    mdnsd_txt_t raop_record;
    mdnsd_txt_t airplay_record;

    char host_name[MDNSD_MAX_NAME];
    char raop_name[MDNSD_MAX_NAME];
    char airplay_name[MDNSD_MAX_NAME];

    unsigned short raop_port;
    unsigned short airplay_port;
    int raop_registered;
    int airplay_registered;

    mdnsd_t *mdnsd;
} dnssd_private_t;

/* "Living room TV (AirPlay)" -> "Living-room-TV-AirPlay.local" */
static void make_host_name(char *dst, size_t dst_len, const char *name, int name_len)
{
    char host[64];
    size_t j = 0;
    int dash = 0;

    for (int i = 0; i < name_len && j < sizeof(host) - 1; i++) {
        unsigned char c = (unsigned char) name[i];
        if (isalnum(c)) {
            host[j++] = (char) c;
            dash = 0;
        } else if (!dash && j > 0) {
            host[j++] = '-';
            dash = 1;
        }
    }
    while (j > 0 && host[j - 1] == '-') {
        j--;
    }
    host[j] = '\0';
    snprintf(dst, dst_len, "%s.local", j ? host : "AirPlay-TV");
}

static int prepare_names(dnssd_private_t *dnssd, dnssd_t *dnssd_public)
{
    char hwaddr[2 * MAX_HWADDR_LEN + 1];

    if (utils_hwaddr_raop(hwaddr, sizeof(hwaddr), dnssd_public->hw_addr, dnssd_public->hw_addr_len) < 0) {
        return -1;
    }
    if (snprintf(dnssd->raop_name, sizeof(dnssd->raop_name), "%s@%s._raop._tcp.local",
                 hwaddr, dnssd_public->name) >= (int) sizeof(dnssd->raop_name)) {
        return -1;
    }
    if (snprintf(dnssd->airplay_name, sizeof(dnssd->airplay_name), "%s._airplay._tcp.local",
                 dnssd_public->name) >= (int) sizeof(dnssd->airplay_name)) {
        return -1;
    }
    make_host_name(dnssd->host_name, sizeof(dnssd->host_name), dnssd_public->name, dnssd_public->name_len);
    return 0;
}

static int build_raop_txt(dnssd_t *dnssd_public)
{
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    char features[22] = {0};
    const char *pw = "false";
    const char *sf = RAOP_SF;

    switch (dnssd_public->pin_pw) {
    case 1:
        pw = "true";
        sf = "0x8c";
        break;
    case 2:
    case 3:
        pw = "true";
        sf = "0x84";
        break;
    default:
        break;
    }

    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd_public->features1, dnssd_public->features2);
    dnssd->raop_record.length = 0;

    return mdnsd_txt_add(&dnssd->raop_record, "ch", RAOP_CH) ||
           mdnsd_txt_add(&dnssd->raop_record, "cn", RAOP_CN) ||
           mdnsd_txt_add(&dnssd->raop_record, "da", RAOP_DA) ||
           mdnsd_txt_add(&dnssd->raop_record, "et", RAOP_ET) ||
           mdnsd_txt_add(&dnssd->raop_record, "vv", RAOP_VV) ||
           mdnsd_txt_add(&dnssd->raop_record, "ft", features) ||
           mdnsd_txt_add(&dnssd->raop_record, "am", GLOBAL_MODEL) ||
           mdnsd_txt_add(&dnssd->raop_record, "md", RAOP_MD) ||
           mdnsd_txt_add(&dnssd->raop_record, "rhd", RAOP_RHD) ||
           mdnsd_txt_add(&dnssd->raop_record, "pw", pw) ||
           mdnsd_txt_add(&dnssd->raop_record, "sr", RAOP_SR) ||
           mdnsd_txt_add(&dnssd->raop_record, "ss", RAOP_SS) ||
           mdnsd_txt_add(&dnssd->raop_record, "sv", RAOP_SV) ||
           mdnsd_txt_add(&dnssd->raop_record, "tp", RAOP_TP) ||
           mdnsd_txt_add(&dnssd->raop_record, "txtvers", RAOP_TXTVERS) ||
           mdnsd_txt_add(&dnssd->raop_record, "sf", sf) ||
           mdnsd_txt_add(&dnssd->raop_record, "vs", RAOP_VS) ||
           mdnsd_txt_add(&dnssd->raop_record, "vn", RAOP_VN) ||
           mdnsd_txt_add(&dnssd->raop_record, "pk", dnssd_public->pk);
}

static int build_airplay_txt(dnssd_t *dnssd_public)
{
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    char device_id[3 * MAX_HWADDR_LEN];
    char features[22] = {0};

    if (utils_hwaddr_airplay(device_id, sizeof(device_id), dnssd_public->hw_addr, dnssd_public->hw_addr_len) < 0) {
        return -1;
    }

    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd_public->features1, dnssd_public->features2);
    dnssd->airplay_record.length = 0;

    return mdnsd_txt_add(&dnssd->airplay_record, "deviceid", device_id) ||
           mdnsd_txt_add(&dnssd->airplay_record, "features", features) ||
           mdnsd_txt_add(&dnssd->airplay_record, "pw", dnssd_public->pin_pw ? "true" : "false") ||
           mdnsd_txt_add(&dnssd->airplay_record, "flags", "0x4") ||
           mdnsd_txt_add(&dnssd->airplay_record, "model", GLOBAL_MODEL) ||
           mdnsd_txt_add(&dnssd->airplay_record, "pk", dnssd_public->pk) ||
           mdnsd_txt_add(&dnssd->airplay_record, "pi", AIRPLAY_PI) ||
           mdnsd_txt_add(&dnssd->airplay_record, "srcvers", AIRPLAY_SRCVERS) ||
           mdnsd_txt_add(&dnssd->airplay_record, "vv", AIRPLAY_VV);
}

static void fill_service(mdnsd_service_t *service, const char *type, const char *name,
                         unsigned short port, const mdnsd_txt_t *txt, int registered)
{
    memset(service, 0, sizeof(*service));
    snprintf(service->type, sizeof(service->type), "%s", type);
    snprintf(service->name, sizeof(service->name), "%s", name);
    service->port = port;
    service->txt = *txt;
    service->registered = registered;
}

static void update_mdnsd(dnssd_private_t *dnssd)
{
    mdnsd_service_t airplay;
    mdnsd_service_t raop;

    fill_service(&airplay, "_airplay._tcp.local", dnssd->airplay_name, dnssd->airplay_port,
                 &dnssd->airplay_record, dnssd->airplay_registered);
    fill_service(&raop, "_raop._tcp.local", dnssd->raop_name, dnssd->raop_port,
                 &dnssd->raop_record, dnssd->raop_registered);
    mdnsd_set_services(dnssd->mdnsd, &airplay, &raop);
}

static int start_and_announce(dnssd_private_t *dnssd)
{
    update_mdnsd(dnssd);
    int ret = mdnsd_start(dnssd->mdnsd);
    if (!ret) {
        mdnsd_announce(dnssd->mdnsd, MDNSD_TTL_SERVICE);
    }
    return ret;
}

void *
dnssd_private_init(dnssd_t *dnssd_public, int *error)
{
    if (error) *error = DNSSD_ERROR_NOERROR;

    dnssd_private_t *dnssd = (dnssd_private_t *) calloc(1, sizeof(dnssd_private_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    if (prepare_names(dnssd, dnssd_public)) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_HWADDRLEN;
        return NULL;
    }
    dnssd->mdnsd = mdnsd_init(dnssd->host_name);
    if (!dnssd->mdnsd) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    return dnssd;
}

void
dnssd_private_destroy(void *private)
{
    if (private) {
        dnssd_private_t *dnssd = (dnssd_private_t *) private;
        mdnsd_destroy(dnssd->mdnsd);
        free(dnssd);
    }
}

int
dnssd_register_raop(dnssd_t *dnssd_public, unsigned short port)
{
    assert(dnssd_public && dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    if (build_raop_txt(dnssd_public)) {
        return -1;
    }
    dnssd->raop_port = port;
    dnssd->raop_registered = 1;
    return start_and_announce(dnssd);
}

int
dnssd_register_airplay(dnssd_t *dnssd_public, unsigned short port)
{
    assert(dnssd_public && dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    if (build_airplay_txt(dnssd_public)) {
        return -1;
    }
    dnssd->airplay_port = port;
    dnssd->airplay_registered = 1;
    return start_and_announce(dnssd);
}

const char *
dnssd_get_raop_txt(dnssd_t *dnssd_public, int *length)
{
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    *length = dnssd->raop_record.length;
    return (const char *) dnssd->raop_record.bytes;
}

const char *
dnssd_get_airplay_txt(dnssd_t *dnssd_public, int *length)
{
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    *length = dnssd->airplay_record.length;
    return (const char *) dnssd->airplay_record.bytes;
}

void
dnssd_unregister_raop(dnssd_t *dnssd_public)
{
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    if (dnssd->raop_registered) {
        mdnsd_service_t service;
        fill_service(&service, "_raop._tcp.local", dnssd->raop_name, dnssd->raop_port,
                     &dnssd->raop_record, 1);
        mdnsd_goodbye(dnssd->mdnsd, &service);
        dnssd->raop_registered = 0;
        update_mdnsd(dnssd);
    }
    if (!dnssd->airplay_registered) {
        mdnsd_stop(dnssd->mdnsd);
    }
}

void
dnssd_unregister_airplay(dnssd_t *dnssd_public)
{
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    if (dnssd->airplay_registered) {
        mdnsd_service_t service;
        fill_service(&service, "_airplay._tcp.local", dnssd->airplay_name, dnssd->airplay_port,
                     &dnssd->airplay_record, 1);
        mdnsd_goodbye(dnssd->mdnsd, &service);
        dnssd->airplay_registered = 0;
        update_mdnsd(dnssd);
    }
    if (!dnssd->raop_registered) {
        mdnsd_stop(dnssd->mdnsd);
    }
}

int
dnssd_get_service_fd(dnssd_t *dnssd_public, int service)
{
    (void) dnssd_public;
    (void) service;
    return -1;
}

int
dnssd_process_service(dnssd_t *dnssd_public, int service)
{
    (void) dnssd_public;
    (void) service;
    return 0;
}

void
dnssd_error_text(int *dnssd_error, const char *appname)
{
    (void) appname;
    fprintf(stderr, "mdnsd (android) error %d: check UDP port 5353 and multicast access\n",
            dnssd_error ? *dnssd_error : 0);
}
