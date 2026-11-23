/*
 * 轻量网络探针：不走 HTTP/DNS，用最小字节和最短射频唤醒时间判断链路是否活跃。
 *
 * 三种能力，全部使用 ESP-IDF 原生 lwIP socket（零新增外部组件）：
 *   1. net_probe_icmp()       单包 ICMP Echo（~100 B 往返），外网首选哨兵
 *   2. net_probe_tcp()        纯 IPv4 非阻塞 TCP 三次握手即关（~350 B），禁 Ping 时降级
 *   3. net_probe_origin_tcp() 解析 http(s)://host[:port] 并探测端口是否可达，
 *                             用于硬件/网线断联后以极低开销监听 Portal 何时恢复
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 向纯 IPv4 地址发送单个最小 ICMP Echo Request 并等待对应 Reply。
 *
 * @param ip         点分十进制 IPv4（不触发 DNS）
 * @param timeout_ms 最长等待毫秒数
 * @return true 收到匹配的 Echo Reply；false 超时、不可达或参数无效
 */
bool net_probe_icmp(const char *ip, int timeout_ms);

/**
 * @brief 向纯 IPv4 地址指定端口发起非阻塞 TCP 握手，连上即关闭。
 *
 * 不发送 TLS/HTTP 数据，仅验证路由与对端端口是否放行。
 */
bool net_probe_tcp(const char *ip, uint16_t port, int timeout_ms);

/**
 * @brief 探测 "http(s)://host[:port]" 的 TCP 端口是否可达。
 *
 * 主机为 IPv4 字面量时不触发 DNS；为域名时通过 getaddrinfo 解析。
 */
bool net_probe_origin_tcp(const char *origin, int timeout_ms);

#ifdef __cplusplus
}
#endif
