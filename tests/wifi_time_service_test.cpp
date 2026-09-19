#include "wifi_scan_coordinator.h"
#include "time_service_logic.h"
#include "wifi_storage_logic.h"

#include <assert.h>
#include <string.h>

int main()
{
    WifiScanCoordinator scan;

    // CONNECT -> SCAN: enqueue does not touch the driver and remains QUEUED.
    const uint32_t first = scan.queue(100);
    assert(first != 0 && scan.phase == WifiScanPhase::QUEUED);
    assert(scan.worker_received(first));
    assert(scan.phase == WifiScanPhase::WAITING_FOR_RADIO);
    assert(!wifi_scan_may_start(true, 4099, 100, 4000));
    assert(wifi_scan_may_start(true, 4100, 100, 4000));
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

    // =========================================================================
    // WiFi Storage & Transactional Lifecycle Contract Tests
    // =========================================================================

    // 1. Checksum & Integrity:
    const uint32_t chk_good = wifi_credentials_checksum("MyRouter", "Secret123");
    assert(chk_good != 0);
    assert(wifi_credentials_verify("MyRouter", "Secret123", chk_good));
    // Different password must change checksum
    assert(!wifi_credentials_verify("MyRouter", "WrongPass", chk_good));
    // Different SSID must change checksum
    assert(!wifi_credentials_verify("OtherRouter", "Secret123", chk_good));
    // Torn write: new SSID paired with old pass must fail checksum
    assert(!wifi_credentials_verify("NewRouter", "Secret123", chk_good));

    // 2. Can Commit Save Rules:
    // Requires: should_save, connected, valid IP, matching generation, not manual disconnect
    assert(wifi_can_commit_save(true, true, true, 5, 5, false));
    // Missing IP (0.0.0.0 during DHCP): CANNOT commit save yet!
    assert(!wifi_can_commit_save(true, true, false, 5, 5, false));
    // Not connected: CANNOT commit save
    assert(!wifi_can_commit_save(true, false, true, 5, 5, false));
    // Not requested to save (e.g. boot auto-connect): CANNOT save
    assert(!wifi_can_commit_save(false, true, true, 5, 5, false));
    // Generation mismatch (stale connect): CANNOT save
    assert(!wifi_can_commit_save(true, true, true, 4, 5, false));
    // Manual disconnect in flight: CANNOT save
    assert(!wifi_can_commit_save(true, true, true, 5, 5, true));

    // 3. Retry preserves save intention:
    // When connection is interrupted or times out, save intent remains active for next retry
    assert(wifi_retry_preserves_save(true, false, false));
    // Manual disconnect cancels save intent
    assert(!wifi_retry_preserves_save(true, true, false));
    // Forget cancels save intent
    assert(!wifi_retry_preserves_save(true, false, true));

    // 4. Failed attempt restores good saved network:
    assert(wifi_failed_attempt_should_restore_saved(true, true));
    assert(!wifi_failed_attempt_should_restore_saved(false, true)); // was not attempting new save
    assert(!wifi_failed_attempt_should_restore_saved(true, false)); // no saved network to restore

    // 5. Sample SSID rejection:
    assert(wifi_is_sample_ssid(""));
    assert(wifi_is_sample_ssid("YourSSID"));
    assert(wifi_is_sample_ssid("MyHomeWiFi"));
    assert(wifi_is_sample_ssid("example"));
    assert(!wifi_is_sample_ssid("The Gioi Kem"));
    assert(!wifi_is_sample_ssid("Văn Phòng 2"));

    // 6. Simulated Transactional Recovery with Backup Slot:
    struct SimulatedNvs
    {
        char ssid[33] = {};
        char pass[65] = {};
        uint32_t chk = 0;
        char b_ssid[33] = {};
        char b_pass[65] = {};
        uint32_t b_chk = 0;

        bool save(const char *new_s, const char *new_p, int simulate_power_loss_step = 0)
        {
            // Step 1: Backup current valid config
            if (strlen(ssid) > 0 && chk != 0 && chk == wifi_credentials_checksum(ssid, pass))
            {
                strcpy(b_ssid, ssid);
                strcpy(b_pass, pass);
                b_chk = chk;
            }
            if (simulate_power_loss_step == 1) return false; // Power cut before write

            // Step 2: Invalidate primary slot
            chk = 0;
            if (simulate_power_loss_step == 2) return false; // Power cut after invalidation

            // Step 3: Write pass
            strcpy(pass, new_p);
            if (simulate_power_loss_step == 3) return false; // Power cut after pass write

            // Step 4: Write ssid
            strcpy(ssid, new_s);
            if (simulate_power_loss_step == 4) return false; // Power cut after ssid write (torn!)

            // Step 5: Write valid checksum
            chk = wifi_credentials_checksum(new_s, new_p);
            return true;
        }

        bool load(char *out_s, char *out_p)
        {
            if (strlen(ssid) > 0)
            {
                const uint32_t calc = wifi_credentials_checksum(ssid, pass);
                if (chk != 0 && chk == calc)
                {
                    strcpy(out_s, ssid);
                    strcpy(out_p, pass);
                    return true;
                }
                // Check backup
                if (strlen(b_ssid) > 0 && b_chk != 0 && b_chk == wifi_credentials_checksum(b_ssid, b_pass))
                {
                    // Restore from backup
                    strcpy(ssid, b_ssid);
                    strcpy(pass, b_pass);
                    chk = b_chk;
                    strcpy(out_s, ssid);
                    strcpy(out_p, pass);
                    return true;
                }
            }
            return false;
        }

        void forget()
        {
            ssid[0] = '\0';
            pass[0] = '\0';
            chk = 0;
            b_ssid[0] = '\0';
            b_pass[0] = '\0';
            b_chk = 0;
        }
    };

    SimulatedNvs nvs;
    char out_s[33] = {}, out_p[65] = {};

    // Initial state: nothing loaded
    assert(!nvs.load(out_s, out_p));

    // Save good network 1:
    assert(nvs.save("GoodNet1", "Pass1"));
    assert(nvs.load(out_s, out_p));
    assert(strcmp(out_s, "GoodNet1") == 0 && strcmp(out_p, "Pass1") == 0);

    // Simulate power loss during step 3 (writing new pass):
    // New pass written ("Pass2"), but chk is 0 (invalidated) and ssid is still "GoodNet1"
    SimulatedNvs nvs_torn = nvs;
    nvs_torn.save("Net2", "Pass2", 3);
    // Loader MUST NOT load torn config (GoodNet1 with Pass2); must recover GoodNet1 with Pass1 from backup!
    assert(nvs_torn.load(out_s, out_p));
    assert(strcmp(out_s, "GoodNet1") == 0 && strcmp(out_p, "Pass1") == 0);

    // Simulate power loss during step 4 (after writing new SSID "Net2", but before checksum):
    SimulatedNvs nvs_torn4 = nvs;
    nvs_torn4.save("Net2", "Pass2", 4);
    assert(nvs_torn4.load(out_s, out_p));
    assert(strcmp(out_s, "GoodNet1") == 0 && strcmp(out_p, "Pass1") == 0);

    // Full successful save:
    assert(nvs.save("GoodNet2", "Pass2", 0));
    assert(nvs.load(out_s, out_p));
    assert(strcmp(out_s, "GoodNet2") == 0 && strcmp(out_p, "Pass2") == 0);

    // Forget network:
    nvs.forget();
    assert(!nvs.load(out_s, out_p));

    return 0;
}
