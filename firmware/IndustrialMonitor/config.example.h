#pragma once

// Copy this file to config.h and enter deployment-specific values.
// Never commit config.h or share credentials/personal contact details.
#define ADMIN_NUMBER       "+10000000000"     // Replace with authorized SMS sender
#define APN                "your.apn.example" // Cellular carrier APN
#define PDP_TYPE           "IPV4V6"

// Example IPv6 uses the documentation-only 2001:db8::/32 prefix.
#define IPV6_HOST_RAW      "2001:db8::1"
#define IPV6_HOST_BRACKETED "[" IPV6_HOST_RAW "]"
#define IPV6_PATH          "/sensor"
#define IPV6_PORT          5000

#define DEVICE_ID          "MONITOR_001"
