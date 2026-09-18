#include "wifi_scan_coordinator.h"
#include "time_service_logic.h"

#include <assert.h>
#include <string.h>

struct FakeScanDriver
{
    bool accept = true;
    uint32_t starts = 0;
    uint32_t active_request = 0;

    bool start(uint32_t request)
    {
        ++starts;
        if (!accept) return false;
        active_request = request;
        return true;
    }
};

int main()
{
    WifiScanCoordinator scan;
    FakeScanDriver driver;

    // CONNECT -> SCAN: enqueue does not touch the driver and remains QUEUED.
    const uint32_t first = scan.queue(100);
    assert(first != 0 && scan.phase == WifiScanPhase::QUEUED);
    assert(driver.starts == 0);
    assert(scan.worker_received(first));
    assert(scan.phase == WifiScanPhase::WAITING_FOR_RADIO);
    assert(!wifi_scan_may_start(true, 4099, 100, 4000));
    assert(wifi_scan_may_start(true, 4100, 100, 4000));
    assert(driver.start(first));
    assert(scan.driver_accepted(first, 4100, 15000));
    assert(scan.finish(first, WifiScanPhase::FAILED)); // failed before UI poll
    const uint32_t failed_revision = scan.result_revision;
    assert(failed_revision != 0 && scan.phase == WifiScanPhase::FAILED);

    // Queue saturation still creates a finite terminal state for the attempted request.
    const uint32_t queue_full = scan.queue(5000);
    assert(queue_full != 0);
    assert(scan.finish(queue_full, WifiScanPhase::FAILED));

    // Wrong-password reconnect cannot starve a manual scan forever.
    const uint32_t manual = scan.queue(6000);
    assert(scan.worker_received(manual));
    assert(wifi_scan_may_start(true, 10000, 6000, 4000));
    assert(scan.driver_accepted(manual, 10000, 15000));
    assert(scan.expired(manual, 25000));
    assert(scan.finish(manual, WifiScanPhase::FAILED));

    // Cancel -> late completion -> new request: old driver result is rejected.
    const uint32_t canceled = scan.queue(26000);
    assert(scan.worker_received(canceled));
    assert(scan.driver_accepted(canceled, 26001, 15000));
    assert(scan.finish(canceled, WifiScanPhase::CANCELED));
    const uint32_t newer = scan.queue(26002);
    assert(newer != canceled);
    assert(!wifi_scan_result_belongs_to(canceled, scan));
    assert(scan.worker_received(newer));
    assert(scan.driver_accepted(newer, 26003, 15000));
    assert(scan.finish(newer, WifiScanPhase::DONE)); // zero AP is valid DONE
    const uint32_t reopen_revision = scan.result_revision;
    assert(reopen_revision > failed_revision); // close/reopen can consume latest snapshot

    // Simulated OOM/driver rejection is terminal and never sticks RUNNING.
    const uint32_t oom = scan.queue(30000);
    assert(scan.worker_received(oom));
    assert(scan.finish(oom, WifiScanPhase::FAILED));
    assert(WifiScanCoordinator::terminal(scan.phase));

    TimeSyncLogic clock;
    assert(!clock.synced);
    assert(!clock.should_request(false, 1));
    assert(clock.should_request(true, 10));
    clock.requested(10);
    clock.observe_epoch(1704067199);
    assert(!clock.synced);
    clock.observe_epoch(1704067200);
    assert(clock.synced);
    assert(!clock.should_request(false, 20)); // offline keeps synchronized epoch state
    assert(clock.synced);
    assert(clock.should_request(true, 30));   // reconnect triggers resync
    char hhmm[6] = {};
    utc7_hhmm(17 * 3600, hhmm);
    assert(strcmp(hhmm, "00:00") == 0);      // UTC+7 midnight rollover
    utc7_hhmm(-7 * 3600, hhmm);
    assert(strcmp(hhmm, "00:00") == 0);      // negative epoch remains bounded
    utc7_hhmm(static_cast<time_t>(INT64_MAX), hhmm);
    assert(strlen(hhmm) == 5 && hhmm[2] == ':');
    return 0;
}
