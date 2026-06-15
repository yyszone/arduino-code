#pragma once
// =============================================================================
// ==  ipv6.h  —  ESP32 IPv6 工具模块 (完全非阻塞高效率版)                      ==
// ==  依赖: <WiFi.h> <WebServer.h> <esp_netif.h>                            ==
// ==  说明: 移除了原有的 while-delay 阻塞设计，改用后台异步更新机制，         ==
// ==        避免在 WiFi 连接/重连时导致主循环及 WS2812 点阵卡顿。             ==
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

  // ── WiFi 连接成功后调用：非阻塞初始化 ──────────────────────────────────
  // 不再进行任何死等，ESP32 协议栈会在后台异步接收 RA 通告并生成 Global IP
  void onWiFiConnected() {
    updateCache();
  }

  // ── 实时获取当前 IPv6 地址（优先 Global > Link-local）──────────────────
  // 每次调用时实时查询，并在获取到有效地址时动态更新缓存（无阻塞设计）
  String getAddress() {
    if (WiFi.status() != WL_CONNECTED) return _cache; // 断线时直接返回上一次的缓存值
    
    // 尝试获取全局单播地址
    String g = _fetchGlobal();
    if (g.length() > 0) {
      _cache = g; // 动态更新缓存
      return g;
    }
    
    // 降级尝试获取链路本地地址
    String ll = _fetchLinkLocal();
    if (ll.length() > 0) {
      _cache = ll; // 动态更新缓存
      return ll;
    }
    
    return _cache;
  }

  // ── 主动尝试更新一次缓存（非阻塞）──────────────────────────────────────
  void updateCache() {
    if (WiFi.status() != WL_CONNECTED) return;
    String g = _fetchGlobal();
    if (g.length() > 0) {
      _cache = g;
    } else {
      String ll = _fetchLinkLocal();
      if (ll.length() > 0) {
        _cache = ll;
      }
    }
  }

  // ── 返回上次缓存的地址（轻量，无系统接口调用）──────────────────────────
  const String &cached() const { return _cache; }

  // ── 将 IPv6 追加到已有 JSON 字符串末尾（逗号紧随其后）──────────────────
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
    if (esp_netif_get_ip6_global(ni, &addr) == ESP_OK) {
      if (addr.addr[0] || addr.addr[1] || addr.addr[2] || addr.addr[3]) {
        return _ipv6_format(&addr);
      }
    }
    return String();
  }

  String _fetchLinkLocal() {
    esp_netif_t *ni = _netif();
    if (!ni) return String();
    esp_ip6_addr_t addr;
    if (esp_netif_get_ip6_linklocal(ni, &addr) == ESP_OK) {
      if (addr.addr[0] || addr.addr[1] || addr.addr[2] || addr.addr[3]) {
        return _ipv6_format(&addr);
      }
    }
    return String();
  }
};

// 全局单例
IPv6Manager ipv6Mgr;