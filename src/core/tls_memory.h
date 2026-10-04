#pragma once

// Call exactly once from setup, before network workers start. Never switch the
// process-wide mbedTLS allocator while a TLS session is alive.
bool tls_memory_init();
