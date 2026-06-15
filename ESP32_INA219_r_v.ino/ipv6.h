#pragma once
// =============================================================================
// ==  ipv6.h  —  ESP32 IPv6 工具模块                                        ==
// ==  依赖: <WiFi.h> <WebServer.h> <esp_netif.h>                            ==
// ==  用法: #include "ipv6.h"                                                ==
// ==        在 setup() 中调用 IPv6.begin(server);                            ==
// ==        在 WiFi 连接成功后调用 IPv6.onWiFiConnected();                  ==
// =============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <esp_netif.h>

// ─────────────────────────────────────────────
// 内部：将 esp_ip6_addr_t 格式化为标准字符串
// ─────────────────────────────────────────────
static String _ipv6_format(const esp_ip6_addr_t *addr) {
  if (!addr) return String("::");
  char buf[40];
  uint32_t w0 = ntohl(addr->addr[0]), w1 = ntohl(addr->addr[1]);
  uint32_t w2 = ntohl(addr->addr[2]), w3 = ntohl(addr->addr[3]);
  snprintf(buf, sizeof(buf),
    "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
    (uint16_t)(w0 >> 16), (uint16_t)(w0 & 0xFFFF),
    (uint16_t)(w1 >> 16), (uint16_t)(w1 & 0xFFFF),
    (uint16_t)(w2 >> 16), (uint16_t)(w2 & 0xFFFF),
    (uint16_t)(w3 >> 16), (uint16_t)(w3 & 0xFFFF));
  return String(buf);
}

// =============================================================================
class IPv6Manager {
public:

  // ── 初始化：在 setup() 里调用，注册 /getIPv6 路由 ──────────────────────
  void begin(WebServer &server) {
    server.on("/getIPv6", HTTP_GET, [this, &server]() {
      server.send(200, "text/plain", getAddress());
    });
  }

  // ── WiFi 连接成功后调用：启用 IPv6 并缓存地址 ───────────────────────────
  // 注意：WiFi.enableIPv6() 必须在 WiFi.begin() 之前调用，
  //       onWiFiConnected() 在 WL_CONNECTED 后调用，等待 RA 下发。
  void onWiFiConnected(unsigned long waitMs = 2000) {
    // 等路由通告(RA)下发全局地址，最多 waitMs 毫秒
    unsigned long t0 = millis();
    while (millis() - t0 < waitMs) {
      String addr = _fetchGlobal();
      if (addr.length() > 0) { _cache = addr; return; }
      delay(200);
    }
    // 超时退而缓存 Link-local
    _cache = _fetchLinkLocal();
  }

  // ── 获取当前 IPv6 地址（优先 Global > Link-local）────────────────────────
  // 每次调用都实时查询，不依赖缓存（适合 handler 里调用）
  String getAddress() {
    String g = _fetchGlobal();
    if (g.length() > 0) return g;
    String ll = _fetchLinkLocal();
    if (ll.length() > 0) return ll;
    return String("Not Available");
  }

  // ── 返回上次 onWiFiConnected() 缓存的地址（轻量，无系统调用）─────────────
  const String &cached() const { return _cache; }

  // ── 将 IPv6 追加到已有 JSON 字符串末尾（逗号紧随其后）──────────────────
  //    用法: appendJSON(json);  然后继续 json += "\"next\":..." ;
  void appendJSON(String &json) {
    json += "\"ipv6\":\"" + getAddress() + "\",";
  }

private:
  String _cache = "Not Available";

  esp_netif_t *_netif() {
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  }

  String _fetchGlobal() {
    esp_netif_t *ni = _netif();
    if (!ni) return String();
    esp_ip6_addr_t addr;
    if (esp_netif_get_ip6_global(ni, &addr) == ESP_OK)
      if (addr.addr[0] || addr.addr[1] || addr.addr[2] || addr.addr[3])
        return _ipv6_format(&addr);
    return String();
  }

  String _fetchLinkLocal() {
    esp_netif_t *ni = _netif();
    if (!ni) return String();
    esp_ip6_addr_t addr;
    if (esp_netif_get_ip6_linklocal(ni, &addr) == ESP_OK)
      if (addr.addr[0] || addr.addr[1] || addr.addr[2] || addr.addr[3])
        return _ipv6_format(&addr);
    return String();
  }
};

// 全局单例（注意：不能命名为 IPv6，与 Arduino IPAddress.h 中的枚举值冲突）
IPv6Manager ipv6Mgr;