Import("env")

import hashlib
import json
import os


EXPECTED_VERSION = "3.20017.241212+sha.dcc1105b"
EXPECTED_HASHES = {
    "WiFiScan.cpp": "e96ac4be873860dc2b441d6d01d6613eb4d06bb21e3f225c3f428619496e6c21",
    "WiFiScan.h": "13f37f842839c97d0ca3c0d68db010f13aa024d3cc3513ffaa359bfb982400e9",
    "WiFiGeneric.cpp": "e37ce5edb3e0ab2f9d6a2879b2c32162c78c7b76249bc0ff114b3283bcaa2e51",
}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            "Arduino WiFi compatibility patch mismatch for %s: expected 1 occurrence, found %d"
            % (label, count)
        )
    return text.replace(old, new, 1)


framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
if not framework_dir:
    raise RuntimeError("Arduino ESP32 framework package is not resolved")

package_json = os.path.join(framework_dir, "package.json")
with open(package_json, "r", encoding="utf-8") as package_file:
    framework_version = json.load(package_file).get("version", "")
if framework_version != EXPECTED_VERSION:
    raise RuntimeError(
        "Unsupported Arduino ESP32 framework %s; WiFi adapter requires %s"
        % (framework_version, EXPECTED_VERSION)
    )

wifi_source_dir = os.path.join(framework_dir, "libraries", "WiFi", "src")
source_paths = {name: os.path.join(wifi_source_dir, name) for name in EXPECTED_HASHES}
for name, expected in EXPECTED_HASHES.items():
    actual = sha256(source_paths[name])
    if actual != expected:
        raise RuntimeError(
            "Arduino WiFi source hash mismatch for %s: expected %s, got %s"
            % (name, expected, actual)
        )

generated_dir = os.path.join(env.subst("$BUILD_DIR"), "framework_wifi_patch")
os.makedirs(generated_dir, exist_ok=True)

with open(source_paths["WiFiScan.cpp"], "r", encoding="utf-8") as source_file:
    scan_source = source_file.read()

scan_source = replace_once(
    scan_source,
    "    wifi_scan_config_t config;\n",
    "    wifi_scan_config_t config = {};\n",
    "zero initialized wifi_scan_config_t",
)
scan_source = replace_once(
    scan_source,
    "    config.show_hidden = show_hidden;\n",
    "    config.show_hidden = show_hidden;\n"
    "    config.home_chan_dwell_time = 30;\n",
    "explicit home channel dwell",
)
scan_source = replace_once(
    scan_source,
    "    if(esp_wifi_scan_start(&config, false) == ESP_OK) {\n"
    "        _scanStarted = millis();\n"
    "        if (!_scanStarted) { //Prevent 0 from millis overflow\n"
    "\t    ++_scanStarted;\n"
    "\t}\n\n"
    "        WiFiGenericClass::clearStatusBits(WIFI_SCAN_DONE_BIT);\n"
    "        WiFiGenericClass::setStatusBits(WIFI_SCANNING_BIT);\n\n",
    "    WiFiGenericClass::clearStatusBits(WIFI_SCAN_DONE_BIT);\n"
    "    WiFiGenericClass::setStatusBits(WIFI_SCANNING_BIT);\n"
    "    _scanStarted = millis();\n"
    "    if (!_scanStarted) { // Prevent 0 from millis overflow\n"
    "        ++_scanStarted;\n"
    "    }\n"
    "    const esp_err_t scan_err = esp_wifi_scan_start(&config, false);\n"
    "    if(scan_err == ESP_OK) {\n",
    "scan bookkeeping before driver start",
)
scan_source = replace_once(
    scan_source,
    "    }\n"
    "    return WIFI_SCAN_FAILED;\n"
    "}\n\n\n"
    "/**\n"
    " * private\n",
    "    }\n"
    "    _scanStarted = 0;\n"
    "    WiFiGenericClass::clearStatusBits(WIFI_SCANNING_BIT);\n"
    "    log_e(\"esp_wifi_scan_start failed: %d (%s)\", scan_err, esp_err_to_name(scan_err));\n"
    "    return WIFI_SCAN_FAILED;\n"
    "}\n\n\n"
    "/**\n"
    " * private\n",
    "preserve scan start error",
)
scan_source = replace_once(
    scan_source,
    "void WiFiScanClass::_scanDone()\n"
    "{\n"
    "    esp_wifi_scan_get_ap_num(&(WiFiScanClass::_scanCount));\n"
    "    if(WiFiScanClass::_scanCount) {\n"
    "        WiFiScanClass::_scanResult = new wifi_ap_record_t[WiFiScanClass::_scanCount];\n"
    "        if(!WiFiScanClass::_scanResult || esp_wifi_scan_get_ap_records(&(WiFiScanClass::_scanCount), (wifi_ap_record_t*)_scanResult) != ESP_OK) {\n"
    "            WiFiScanClass::_scanCount = 0;\n"
    "        }\n"
    "    }\n"
    "    WiFiScanClass::_scanStarted=0; //Reset after a scan is completed for normal behavior\n"
    "    WiFiGenericClass::setStatusBits(WIFI_SCAN_DONE_BIT);\n"
    "    WiFiGenericClass::clearStatusBits(WIFI_SCANNING_BIT);\n"
    "}\n",
    "void WiFiScanClass::_scanDone()\n"
    "{\n"
    "    static constexpr uint16_t MAX_MANAGED_SCAN_RESULTS = 64;\n"
    "    uint16_t available = 0;\n"
    "    const esp_err_t count_err = esp_wifi_scan_get_ap_num(&available);\n"
    "    WiFiScanClass::_scanCount = 0;\n"
    "    if(count_err == ESP_OK && available) {\n"
    "        WiFiScanClass::_scanCount = available > MAX_MANAGED_SCAN_RESULTS\n"
    "            ? MAX_MANAGED_SCAN_RESULTS : available;\n"
    "        WiFiScanClass::_scanResult = new (std::nothrow) wifi_ap_record_t[WiFiScanClass::_scanCount];\n"
    "        if(!WiFiScanClass::_scanResult) {\n"
    "            log_e(\"WiFi scan result allocation failed for %u APs\", WiFiScanClass::_scanCount);\n"
    "            WiFiScanClass::_scanCount = 0;\n"
    "        } else {\n"
    "            const esp_err_t records_err = esp_wifi_scan_get_ap_records(\n"
    "                &(WiFiScanClass::_scanCount), (wifi_ap_record_t*)_scanResult);\n"
    "            if(records_err != ESP_OK) {\n"
    "                log_e(\"esp_wifi_scan_get_ap_records failed: %d (%s)\", records_err, esp_err_to_name(records_err));\n"
    "                delete[] reinterpret_cast<wifi_ap_record_t*>(WiFiScanClass::_scanResult);\n"
    "                WiFiScanClass::_scanResult = 0;\n"
    "                WiFiScanClass::_scanCount = 0;\n"
    "            }\n"
    "        }\n"
    "    } else if(count_err != ESP_OK) {\n"
    "        log_e(\"esp_wifi_scan_get_ap_num failed: %d (%s)\", count_err, esp_err_to_name(count_err));\n"
    "    }\n"
    "    WiFiScanClass::_scanStarted=0;\n"
    "    WiFiGenericClass::setStatusBits(WIFI_SCAN_DONE_BIT);\n"
    "    WiFiGenericClass::clearStatusBits(WIFI_SCANNING_BIT);\n"
    "}\n",
    "bounded framework result ownership",
)
scan_source = replace_once(
    scan_source,
    "    if (WiFiScanClass::_scanStarted && (millis()-WiFiScanClass::_scanStarted) > WiFiScanClass::_scanTimeout) { //Check is scan was started and if the delay expired, return WIFI_SCAN_FAILED in this case \n"
    "    \tWiFiGenericClass::clearStatusBits(WIFI_SCANNING_BIT);\n"
    "\treturn WIFI_SCAN_FAILED;\n"
    "    }\n",
    "    if (WiFiScanClass::_scanStarted && (millis()-WiFiScanClass::_scanStarted) > WiFiScanClass::_scanTimeout) {\n"
    "        const esp_err_t stop_err = esp_wifi_scan_stop();\n"
    "        log_w(\"WiFi scan wrapper timeout; stop=%d (%s)\", stop_err, esp_err_to_name(stop_err));\n"
    "        return WIFI_SCAN_FAILED;\n"
    "    }\n",
    "wrapper timeout keeps driver lifecycle",
)
scan_source = replace_once(
    scan_source,
    "#include <string.h>\n",
    "#include <string.h>\n#include <new>\n",
    "nothrow include",
)

with open(source_paths["WiFiGeneric.cpp"], "r", encoding="utf-8") as source_file:
    generic_source = source_file.read()
generic_source = replace_once(
    generic_source,
    "        else if(first_connect) {                                                    //Retry once for all failure reasons\n"
    "            first_connect = false;\n"
    "            DoReconnect = true;\n"
    "            log_d(\"WiFi Reconnect Running\");\n"
    "        }\n",
    "        else if(first_connect) {\n"
    "            // Mini OS owns reconnect sequencing; do not race its scan/connect worker.\n"
    "            first_connect = false;\n"
    "            log_d(\"Initial reconnect delegated to application worker\");\n"
    "        }\n",
    "disable hidden first-connect retry",
)

generated = {}
for name, content in (("WiFiScan.cpp", scan_source), ("WiFiGeneric.cpp", generic_source)):
    output_path = os.path.join(generated_dir, name)
    with open(output_path, "w", encoding="utf-8", newline="\n") as output_file:
        output_file.write(content)
    generated[os.path.normcase(os.path.abspath(source_paths[name]))] = output_path


def use_managed_wifi_source(node):
    source_path = os.path.normcase(os.path.abspath(node.srcnode().get_abspath()))
    replacement = generated.get(source_path)
    return env.File(replacement) if replacement else node


env.AddBuildMiddleware(use_managed_wifi_source)
print("Managed Arduino WiFi scan patch: verified %s and enabled" % framework_version)
