#ifndef DOIP_PROTOCOL_H
#define DOIP_PROTOCOL_H

#include <stdint.h>

// ============================================================
//  DoIP 协议版本
// ============================================================
#define DOIP_PROTOCOL_VERSION       0x02

// ============================================================
//  DoIP 标准端口
// ============================================================
#define DOIP_UDP_DISCOVERY_PORT     13400   // 车辆发现（广播）
#define DOIP_TCP_DATA_PORT          13400   // 诊断数据通信

// ============================================================
//  DoIP 载荷类型（Payload Type）— ISO 13400-2 表5
// ============================================================
enum doip_payload_type {
    // --- 通用握手 ---
    DOIP_VEHICLE_ID_REQ          = 0x0001,   // 车辆识别请求
    DOIP_VEHICLE_ID_REQ_EID      = 0x0002,   // 按 EID 车辆识别请求
    DOIP_VEHICLE_ID_REQ_VIN      = 0x0003,   // 按 VIN 车辆识别请求
    DOIP_VEHICLE_ANNOUNCE        = 0x0004,   // 车辆公告消息

    // --- 路由激活 ---
    DOIP_ROUTING_ACTIVE_REQ      = 0x0005,   // 路由激活请求
    DOIP_ROUTING_ACTIVE_RESP     = 0x0006,   // 路由激活响应

    // --- 诊断消息 ---
    DOIP_DIAG_MESSAGE            = 0x8001,   // 诊断消息（双向）
    DOIP_DIAG_MESSAGE_POS_ACK    = 0x8002,   // 诊断消息肯定应答
    DOIP_DIAG_MESSAGE_NEG_ACK    = 0x8003,   // 诊断消息否定应答

    // --- Alive Check ---
    DOIP_ALIVE_CHECK_REQ         = 0x0007,   // 存活检查请求
    DOIP_ALIVE_CHECK_RESP        = 0x0008,   // 存活检查响应
};

// ============================================================
//  DoIP 通用头部 — 每个 DoIP 消息的前 8 字节
// ============================================================
#pragma pack(push, 1)
typedef struct {
    uint8_t  protocol_version;      // 协议版本（0x02）
    uint8_t  inverse_version;       // 取反版本（0xFD = ~0x02）
    uint16_t payload_type;          // 载荷类型（见上枚举）
    uint32_t payload_length;        // 载荷长度（不含这 8 字节头）
} doip_header_t;

// ============================================================
//  功能寻址：目标地址 >= 此值即为广播，否定应答需抑制
// ============================================================
#define DOIP_FUNCTIONAL_ADDR_BASE 0xE000
#define IS_FUNCTIONAL_ADDR(addr)  ((addr) >= DOIP_FUNCTIONAL_ADDR_BASE)

// ============================================================
//  车辆公告消息载荷
// ============================================================
typedef struct {
    uint8_t  vin[17];               // VIN 码（17字节）
    uint16_t logical_address;       // 逻辑地址
    uint8_t  eid[6];                // EID（6字节）
    uint8_t  gid[6];                // GID（6字节）
    uint8_t  further_action;        // 进一步动作
    uint8_t  sync_status;           // 同步状态
} doip_vehicle_announce_t;

// ============================================================
//  车辆识别请求载荷
// ============================================================
typedef struct {
    // 空载荷，仅用于类型区分
} doip_vehicle_id_req_t;

// ============================================================
//  车辆识别响应载荷
// ============================================================
typedef struct {
    uint8_t  vin[17];
    uint16_t logical_address;
    uint8_t  eid[6];
    uint8_t  gid[6];
    uint8_t  further_action;
} doip_vehicle_id_resp_t;

// ============================================================
//  路由激活请求载荷 — ISO 13400-2 表9
//  source_address(2) + activation_type(1) + reserved(4) = 7字节
// ============================================================
typedef struct {
    uint16_t source_address;        // 测试仪源地址
    uint8_t  activation_type;       // 激活类型: 0x00=默认, 0x01=WWH-OBD
    uint8_t  reserved[4];           // 保留（ISO 13400）
} doip_routing_active_req_t;

// ============================================================
//  路由激活响应载荷 — ISO 13400-2 表11 标准格式
//  tester_logical_address(2) + target_logical_address(2) + response_code(1) + reserved(4)
// ============================================================
typedef struct {
    uint16_t tester_logical_address;    // 测试仪逻辑地址
    uint16_t target_logical_address;    // 目标（本ECU）逻辑地址
    uint8_t  response_code;             // 响应码
    uint8_t  reserved[4];               // 保留
} doip_routing_active_resp_t;

// ============================================================
//  诊断消息载荷
// ============================================================
typedef struct {
    uint16_t source_address;        // 源地址
    uint16_t target_address;        // 目标地址
    uint8_t  user_data[];           // UDS 诊断数据（柔性数组）
} doip_diag_message_t;

// ============================================================
//  诊断消息 ACK/NACK 载荷
// ============================================================
typedef struct {
    uint16_t source_address;
    uint16_t target_address;
    uint8_t  ack_code;              // 0x00 = ACK, 其他 = NACK
    uint8_t  reserved;
} doip_diag_ack_t;

// 路由激活响应码 — ISO 13400-2 表12
#define ROUTING_SUCCESS                 0x10   // 路由激活成功
#define ROUTING_UNKNOWN_SRC             0x00   // 未知源地址 → 关闭 socket
#define ROUTING_NO_SOCKET               0x01   // 所有 socket 已激活 → 关闭
#define ROUTING_SA_DIFFERENT            0x02   // SA 与连接表不一致 → 关闭
#define ROUTING_SA_ON_OTHER_SOCKET      0x03   // SA 已在其他 socket 注册 → 关闭
#define ROUTING_AUTH_REQUIRED           0x04   // 需要认证 → 关闭
#define ROUTING_CONFIRM_REQUIRED        0x05   // 需要确认
#define ROUTING_INVALID_ACT_TYPE        0x06   // 激活类型无效 → 关闭

#pragma pack(pop)

#endif // DOIP_PROTOCOL_H