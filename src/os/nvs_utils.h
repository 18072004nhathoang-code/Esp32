#pragma once

// Check whether an NVS namespace already exists without asking Arduino
// Preferences to open a missing read-only namespace (which logs an error).
bool nvs_namespace_exists(const char *name);
