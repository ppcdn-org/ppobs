#include "device-registration.h"
#include "whip-utils.h"

#include <obs-module.h>
#include <obs.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#endif

#define dev_log(level, format, ...) blog(level, "[obs-webrtc] [device-registration] " format, ##__VA_ARGS__)

namespace {

// ── Hardware info collection ─────────────────────────────────────────

#ifdef _WIN32

// Attempts to read the physical disk serial number via IOCTL_STORAGE_QUERY_PROPERTY
// on \\.\PhysicalDrive0. Returns the serial string or empty on failure.
std::string GetDiskSerialNumber()
{
	HANDLE h = CreateFileA("\\\\.\\PhysicalDrive0", 0,
			       FILE_SHARE_READ | FILE_SHARE_WRITE,
			       nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return "";

	STORAGE_PROPERTY_QUERY query{};
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;

	char buf[4096]{};
	DWORD bytes = 0;
	bool ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
				  &query, sizeof(query),
				  buf, sizeof(buf), &bytes, nullptr);
	CloseHandle(h);
	if (!ok)
		return "";

	auto *descriptor = PSTORAGE_DEVICE_DESCRIPTOR(buf);
	if (!descriptor->SerialNumberOffset)
		return "";

	const char *serial = reinterpret_cast<const char *>(buf) + descriptor->SerialNumberOffset;
	std::string result;
	for (const char *p = serial; *p; ++p) {
		if (*p != ' ' && *p != '\t')
			result.push_back(static_cast<char>(toupper(*p)));
	}
	return result;
}

// Fallback: volume serial number from GetVolumeInformation on C:\.
std::string GetVolumeSerialNumber()
{
	DWORD serial = 0;
	if (!GetVolumeInformationA("C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
		return "";
	char buf[16];
	snprintf(buf, sizeof(buf), "%08lX", serial);
	return buf;
}

// Reads the first non-loopback physical MAC address.
std::string GetLanMacAddress()
{
	ULONG buflen = 0;
	if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST |
				GAA_FLAG_SKIP_MULTICAST |
				GAA_FLAG_SKIP_DNS_SERVER,
				nullptr, nullptr, &buflen) != ERROR_BUFFER_OVERFLOW)
		return "";
	if (buflen == 0)
		return "";

	std::vector<BYTE> buf(buflen);
	auto *adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
	if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST |
				 GAA_FLAG_SKIP_MULTICAST |
				 GAA_FLAG_SKIP_DNS_SERVER,
				 nullptr, adapters, &buflen) != ERROR_SUCCESS)
		return "";

	for (auto *a = adapters; a; a = a->Next) {
		if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		if (a->PhysicalAddressLength == 0)
			continue;
		char mac[18];
		snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X",
			 a->PhysicalAddress[0], a->PhysicalAddress[1],
			 a->PhysicalAddress[2], a->PhysicalAddress[3],
			 a->PhysicalAddress[4], a->PhysicalAddress[5]);
		return mac;
	}
	return "";
}

#endif // _WIN32

// Collects hardware identity: returns disk serial if available, otherwise
// LAN MAC, otherwise empty.
std::string CollectHardwareId()
{
#ifdef _WIN32
	std::string id = GetDiskSerialNumber();
	if (!id.empty()) {
		dev_log(LOG_DEBUG, "collected disk serial (length=%zu)", id.size());
		return id;
	}
	id = GetVolumeSerialNumber();
	if (!id.empty()) {
		dev_log(LOG_DEBUG, "collected volume serial");
		return id;
	}
	id = GetLanMacAddress();
	if (!id.empty()) {
		dev_log(LOG_DEBUG, "collected LAN MAC");
		return id;
	}
	dev_log(LOG_WARNING, "failed to collect any hardware identifier");
#endif
	return "";
}

// ── HTTP helpers ─────────────────────────────────────────────────────

constexpr long DEVICE_REG_TIMEOUT_SECONDS = 8;
constexpr size_t MAX_RESPONSE_SIZE = 8 * 1024;

struct ResponseBuf {
	std::string data;
	bool exceeded = false;
};

size_t dev_write_response(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *buf = static_cast<ResponseBuf *>(userdata);
	const size_t len = size * nmemb;
	if (buf->data.size() + len > MAX_RESPONSE_SIZE) {
		buf->exceeded = true;
		return 0;
	}
	buf->data.append(ptr, len);
	return len;
}

// Derives the device registration URL from the publish URL.
// e.g. "https://host/v1/publish/requests" → "https://host/v1/ppobs-devices/register"
std::string DeriveDeviceRegUrl(const std::string &publishUrl)
{
	const std::string marker = "/v1/";
	auto pos = publishUrl.find(marker);
	if (pos == std::string::npos)
		return "";
	return publishUrl.substr(0, pos + marker.size()) + "ppobs-devices/register";
}

} // namespace

std::string RegisterPpobsDevice(const std::string &ppcenterUrl,
				const std::string &appId,
				const std::string &appSecret)
{
	if (ppcenterUrl.empty() || appId.empty() || appSecret.empty())
		return "";

	const std::string hardwareId = CollectHardwareId();
	if (hardwareId.empty())
		return "";

	const std::string regUrl = DeriveDeviceRegUrl(ppcenterUrl);
	if (regUrl.empty()) {
		dev_log(LOG_ERROR, "cannot derive device registration URL from: %s", ppcenterUrl.c_str());
		return "";
	}

	nlohmann::json body;
	// Send both SN and MAC if available; the server picks whichever
	// is non-empty to compute the deviceId (see ppcenter's
	// user_ppobs_device_store.go: deviceID()).
	body["appId"] = appId;
	body["appSecret"] = appSecret;
	body["deviceSn"] = hardwareId;
#ifdef _WIN32
	{
		std::string mac = GetLanMacAddress();
		if (!mac.empty())
			body["deviceMac"] = mac;
	}
#endif
	const std::string jsonBody = body.dump();

	CURL *curl = curl_easy_init();
	if (!curl)
		return "";

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	const std::string userAgent = generate_user_agent();
	headers = curl_slist_append(headers, userAgent.c_str());

	ResponseBuf buffer;
	char curl_error[CURL_ERROR_SIZE] = {};

	curl_easy_setopt(curl, CURLOPT_URL, regUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dev_write_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, DEVICE_REG_TIMEOUT_SECONDS);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	const CURLcode rc = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK) {
		dev_log(LOG_WARNING, "device registration HTTP request failed: %s",
			buffer.exceeded ? "response too large" :
			(curl_error[0] ? curl_error : curl_easy_strerror(rc)));
		return "";
	}
	if (status != 200) {
		dev_log(LOG_WARNING, "device registration rejected by ppcenter (HTTP %ld)", status);
		return "";
	}

	// Response: {"code":200,"msg":"OK","data":{...device record...}}
	// We extract deviceId from the response data.
	try {
		auto decoded = nlohmann::json::parse(buffer.data);
		if (!decoded.contains("data") || !decoded["data"].is_object())
			return "";
		std::string deviceId = decoded["data"].value("deviceId", "");
		if (deviceId.empty()) {
			dev_log(LOG_WARNING, "device registration response missing deviceId");
			return "";
		}
		dev_log(LOG_INFO, "device registered: deviceId=%s", deviceId.c_str());
		return deviceId;
	} catch (const std::exception &e) {
		dev_log(LOG_WARNING, "device registration response parse failed: %s", e.what());
		return "";
	}
}
