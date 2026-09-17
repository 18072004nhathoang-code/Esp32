# Production security provisioning

The normal PlatformIO build is a development image. It does not provision
Secure Boot, Flash Encryption, or encrypted NVS. Ignoring `include/secrets.h`
in Git prevents accidental source commits only; it does not protect WiFi/API
credentials from a physical flash dump.

Before production deployment:

1. Move secrets out of compile-time defaults and provision unique per-device
   credentials through an authenticated manufacturing flow.
2. Enable ESP32-S3 Secure Boot and Flash Encryption using Espressif's supported
   provisioning process, with test devices first. These eFuse operations can be
   irreversible and must not be enabled opportunistically by application code.
3. Use encrypted NVS for WiFi and application secrets. Keep encryption keys out
   of firmware images and manufacturing logs.
4. Define key ownership, offline backups, rotation, revocation, recovery and
   device-return procedures. Record eFuse state and firmware signing key version
   per device without recording plaintext secrets.
5. Replace or rebuild the current Arduino-ESP32 2.0.17 SDK with an audited TLS
   configuration where X.509 validity dates are checked. Require successful SNTP
   synchronization and a sane UTC time before opening AI, Maps or Camera TLS
   sessions. Test expired, not-yet-valid, wrong-host and untrusted-chain cases.
6. Keep verified HTTPS as the default. Unverified HTTPS and plaintext HTTP are
   explicit diagnostic opt-ins only; never downgrade automatically.

The current prebuilt ESP32-S3 SDK has `CONFIG_MBEDTLS_HAVE_TIME_DATE` disabled.
It validates configured trust chains and hostnames but cannot enforce certificate
not-before/not-after dates. Boot logs state this limitation explicitly.
