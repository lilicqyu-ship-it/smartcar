/*
 * captive_dns.h - wildcard DNS responder for the captive portal
 */
#ifndef S3_CAPTIVE_DNS_H
#define S3_CAPTIVE_DNS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the UDP:53 wildcard responder answering every A query with ap_ip. */
esp_err_t captive_dns_start(const char *ap_ip);

/* Update the answer address (also needed before start). */
void captive_dns_set_ip(const char *ip);

#ifdef __cplusplus
}
#endif

#endif /* S3_CAPTIVE_DNS_H */
