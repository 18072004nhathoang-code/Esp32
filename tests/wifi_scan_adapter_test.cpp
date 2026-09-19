#include "wifi_scan_adapter_logic.h"
#include "wifi_scan_coordinator.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

struct FakeNativeScanConfig
{
    uintptr_t ssid;
    uintptr_t bssid;
    uint8_t channel;
    uint8_t show_hidden;
    uint8_t scan_type;
    uint8_t active_min;
    uint8_t active_max;
    uint8_t passive;
    uint8_t home_dwell;
    uint8_t future_sdk_field[17];
};

struct FakeScanDriver
{
    WifiScanAdapterLogic logic;
    int32_t start_error = 0;
    bool start_may_be_busy = false;
    int32_t stop_error = 0;
    bool stop_may_be_busy = true;

    bool start(uint32_t request, uint32_t now, bool complete_before_return = false)
    {
        assert(logic.prepare_start(request, now));
        if (complete_before_return) logic.on_scan_done(7, 0, 4);
        return logic.start_returned(request, start_error, start_may_be_busy, now + 1);
    }

    void stop(uint32_t request, uint32_t now)
    {
        assert(logic.begin_stop(request, now));
        logic.stop_returned(request, stop_may_be_busy, now + 1);
    }
};

static void test_zero_initialized_config()
{
    FakeNativeScanConfig config;
    memset(&config, 0xa5, sizeof(config));
    wifi_scan_zero_config(config);
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&config);
    for (size_t i = 0; i < sizeof(config); ++i) assert(bytes[i] == 0);

    const WifiScanSettings settings;
    assert(settings.show_hidden);
    assert(settings.active_min_ms == 40);
    assert(settings.active_max_ms == 120);
    assert(settings.home_channel_dwell_ms == 30);
}

static void test_completion_before_start_returns()
{
    FakeScanDriver driver;
    assert(driver.start(11, 100, true));
    assert(driver.logic.phase == WifiScanDriverPhase::COMPLETED);
    WifiScanDriverCompletion completion;
    assert(driver.logic.take_completion(11, completion));
    assert(completion.scan_id == 7 && completion.result_count == 4);
    assert(driver.logic.release_completion(11));
}

static void test_wrapper_timeout_does_not_end_driver_scan()
{
    FakeScanDriver driver;
    WifiScanCoordinator request;
    const uint32_t id = request.queue(1000);
    assert(request.worker_received(id));
    assert(driver.start(id, 1001));
    assert(request.driver_accepted(id, 1001, 10000));

    // Arduino's former max_ms_per_chan*20 timeout would fire at 6000 ms.
    assert(!request.expired(id, 7001));
    assert(driver.logic.phase == WifiScanDriverPhase::RUNNING);
    assert(request.expired(id, 11001));
}

static void test_stop_drain_late_event_and_rescan()
{
    FakeScanDriver driver;
    assert(driver.start(21, 0));
    driver.stop(21, 50);
    assert(driver.logic.phase == WifiScanDriverPhase::DRAINING);
    assert(!driver.logic.drain_expired(2049, 2000));
    assert(driver.logic.drain_expired(2051, 2000));

    // A late completion is discarded and only unlocks the driver lifecycle.
    assert(!driver.logic.on_scan_done(8, 1, 0));
    assert(driver.logic.take_drained_event());
    assert(driver.logic.phase == WifiScanDriverPhase::IDLE);
    assert(driver.start(22, 2100));
}

static void test_existing_driver_scan_is_not_accepted_as_new()
{
    FakeScanDriver driver;
    driver.start_error = 0x3006; // ESP_ERR_WIFI_STATE in the target IDF.
    driver.start_may_be_busy = true;
    assert(!driver.start(31, 100));
    assert(driver.logic.phase == WifiScanDriverPhase::DRAINING);
    assert(driver.logic.active_request_id == 0);

    // The old SCAN_DONE drains state; it is never surfaced as request 31.
    assert(!driver.logic.on_scan_done(44, 0, 9));
    WifiScanDriverCompletion completion;
    assert(!driver.logic.take_completion(31, completion));
    assert(driver.logic.take_drained_event());

    driver.start_error = 0;
    driver.start_may_be_busy = false;
    assert(driver.start(31, 500));
}

static void test_cancel_oom_zero_ap_and_ui_revision()
{
    WifiScanCoordinator scan;
    const uint32_t oom = scan.queue(1);
    assert(scan.queue(1) == 0); // A full/busy coordinator rejects another request.
    assert(scan.worker_received(oom));
    assert(scan.finish(oom, WifiScanPhase::FAILED));
    const uint32_t failed_revision = scan.result_revision;

    const uint32_t zero_ap = scan.queue(2);
    assert(scan.worker_received(zero_ap));
    assert(scan.driver_accepted(zero_ap, 3, 100));
    assert(scan.finish(zero_ap, WifiScanPhase::DONE));
    assert(scan.result_revision > failed_revision); // UI may miss RUNNING.

    const uint32_t canceled = scan.queue(4);
    assert(scan.worker_received(canceled));
    assert(scan.driver_accepted(canceled, 5, 100));
    assert(scan.finish(canceled, WifiScanPhase::CANCELED));
    assert(!wifi_scan_result_belongs_to(canceled, scan));

    assert(wifi_scan_bounded_result_count(0, 32) == 0);
    assert(wifi_scan_bounded_result_count(7, 32) == 7);
    assert(wifi_scan_bounded_result_count(255, 32) == 32);
}

static void test_stop_failure_is_terminal_or_drained()
{
    FakeScanDriver driver;
    assert(driver.start(41, 100));
    driver.stop_may_be_busy = false;
    driver.stop(41, 110);
    assert(driver.logic.phase == WifiScanDriverPhase::IDLE);
    assert(driver.logic.take_drained_event());
    assert(driver.start(42, 120));
}

static void test_connect_deferral_wrong_password_and_wraparound()
{
    WifiScanCoordinator scan;
    const uint32_t id = scan.queue(UINT32_MAX - 1000U);
    assert(scan.worker_received(id));

    // A failed/ongoing connect gets a bounded window, then scan owns radio.
    assert(!wifi_scan_may_start(true, UINT32_MAX - 2U,
                                UINT32_MAX - 1000U, 4000));
    assert(wifi_scan_may_start(true, 3500U,
                               UINT32_MAX - 1000U, 4000));
    assert(!wifi_scan_total_expired(3500U, UINT32_MAX - 1000U, 20000));
    assert(wifi_scan_total_expired(20000U, UINT32_MAX - 1000U, 20000));
}

static void test_recovery_and_stale_events()
{
    FakeScanDriver driver;
    driver.start_error = 0x3006;
    driver.start_may_be_busy = true;
    assert(!driver.start(50, 10));
    assert(driver.logic.begin_recovery(2011));
    driver.logic.recovery_finished(true);
    assert(driver.logic.phase == WifiScanDriverPhase::IDLE);
    assert(driver.logic.take_drained_event());

    assert(!driver.logic.on_scan_done(99, 0, 1));
    assert(driver.logic.stale_event_count == 1);
}

static void test_recovery_failure_bounded_retry()
{
    FakeScanDriver driver;
    driver.start_error = 0x3006;
    driver.start_may_be_busy = true;
    assert(!driver.start(60, 10));
    assert(driver.logic.begin_recovery(100));

    // First recovery failure puts driver into DRAINING (retry backoff), not permanent lockup
    driver.logic.recovery_finished(false, 110);
    assert(driver.logic.phase == WifiScanDriverPhase::DRAINING);
    assert(driver.logic.recovery_retries == 1);

    // Second recovery failure
    assert(driver.logic.begin_recovery(200));
    driver.logic.recovery_finished(false, 210);
    assert(driver.logic.phase == WifiScanDriverPhase::DRAINING);
    assert(driver.logic.recovery_retries == 2);

    // Third recovery failure reaches kMaxRecoveryRetries: enters RECOVERY_FAILED (not fake IDLE)
    assert(driver.logic.begin_recovery(300));
    driver.logic.recovery_finished(false, 310);
    assert(driver.logic.phase == WifiScanDriverPhase::RECOVERY_FAILED);
    assert(!driver.logic.prepare_start(61, 350)); // Cannot start scan while in RECOVERY_FAILED

    // Retrying recovery from RECOVERY_FAILED
    assert(driver.logic.retry_failed_recovery(360));
    driver.logic.recovery_finished(true, 370);
    assert(driver.logic.phase == WifiScanDriverPhase::IDLE);
    assert(driver.logic.take_drained_event());

    // Subsequent scan can now be started normally
    driver.start_error = 0;
    driver.start_may_be_busy = false;
    assert(driver.start(61, 400));
    assert(driver.logic.phase == WifiScanDriverPhase::RUNNING);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    puts("zero_initialized_config");
    test_zero_initialized_config();
    puts("completion_before_start_returns");
    test_completion_before_start_returns();
    puts("wrapper_timeout_does_not_end_driver_scan");
    test_wrapper_timeout_does_not_end_driver_scan();
    puts("stop_drain_late_event_and_rescan");
    test_stop_drain_late_event_and_rescan();
    puts("existing_driver_scan_is_not_accepted_as_new");
    test_existing_driver_scan_is_not_accepted_as_new();
    puts("cancel_oom_zero_ap_and_ui_revision");
    test_cancel_oom_zero_ap_and_ui_revision();
    puts("connect_deferral_wrong_password_and_wraparound");
    test_connect_deferral_wrong_password_and_wraparound();
    puts("stop_failure_is_terminal_or_drained");
    test_stop_failure_is_terminal_or_drained();
    puts("recovery_and_stale_events");
    test_recovery_and_stale_events();
    puts("recovery_failure_bounded_retry");
    test_recovery_failure_bounded_retry();
    puts("wifi_scan_adapter_test PASS");
    return 0;
}
