#pragma once

#include <Arduino.h>

/** Real-time audio always wins over map/camera bulk transfers. */
bool network_coordinator_init(void);
bool network_background_allowed(void);
bool network_bulk_acquire(uint32_t timeout_ms);
void network_bulk_release(void);

class NetworkBulkLease
{
public:
    explicit NetworkBulkLease(uint32_t timeout_ms) : acquired_(network_bulk_acquire(timeout_ms)) {}
    ~NetworkBulkLease() { if (acquired_) network_bulk_release(); }
    bool acquired() const { return acquired_; }
    NetworkBulkLease(const NetworkBulkLease &) = delete;
    NetworkBulkLease &operator=(const NetworkBulkLease &) = delete;
private:
    bool acquired_;
};
