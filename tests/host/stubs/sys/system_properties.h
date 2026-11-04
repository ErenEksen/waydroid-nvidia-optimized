/* host stand-in for bionic <sys/system_properties.h> (tests/host only) */
#pragma once
#define PROP_VALUE_MAX 92
int __system_property_get(const char *name, char *value);
