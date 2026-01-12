#ifndef FAST_DATA_COLLECTOR__ZMQ_PUBLISHER_HPP_
#define FAST_DATA_COLLECTOR__ZMQ_PUBLISHER_HPP_

#include <zmq.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstring>

namespace fast_data_collector {

/**
 * @brief ZeroMQ 发布器封装类
 * 
 * 支持高性能的进程间通信，用于将 C++ 采集的数据传输到 Python 写盘进程
 */
class ZmqPublisher {
public:
    /**
     * @brief 构造函数
     * @param endpoint ZMQ 端点地址，如 "ipc:///tmp/rgb_queue" 或 "tcp://*:5555"
     */
    explicit ZmqPublisher(const std::string& endpoint);
    
    ~ZmqPublisher();
    
    // 禁止拷贝
    ZmqPublisher(const ZmqPublisher&) = delete;
    ZmqPublisher& operator=(const ZmqPublisher&) = delete;
    
    /**
     * @brief 发送二进制数据
     * @param data 数据指针
     * @param size 数据大小
     * @param flags ZMQ 发送标志（默认非阻塞）
     * @return 是否发送成功
     */
    bool send(const void* data, size_t size, int flags = ZMQ_DONTWAIT);
    
    /**
     * @brief 发送 vector 数据
     * @param data 数据 vector
     * @param flags ZMQ 发送标志
     * @return 是否发送成功
     */
    bool send(const std::vector<uint8_t>& data, int flags = ZMQ_DONTWAIT);
    
    /**
     * @brief 发送字符串
     * @param data 字符串数据
     * @param flags ZMQ 发送标志
     * @return 是否发送成功
     */
    bool send(const std::string& data, int flags = ZMQ_DONTWAIT);
    
    /**
     * @brief 检查连接状态
     */
    bool is_connected() const { return connected_; }
    
    /**
     * @brief 获取统计信息
     */
    size_t get_sent_count() const { return sent_count_; }
    size_t get_failed_count() const { return failed_count_; }

private:
    void* context_;
    void* socket_;
    std::string endpoint_;
    bool connected_;
    std::mutex mutex_;
    
    // 统计
    size_t sent_count_;
    size_t failed_count_;
};

/**
 * @brief 消息类型定义
 */
enum class MessageType : uint8_t {
    RGB_FRAME = 1,      // RGB 图像帧
    SLAM_POSE = 2,      // SLAM 位姿
    VIVE_POSE = 3,      // Vive 位姿
    TOF_POINTCLOUD = 4, // ToF 点云
    CLAMP_DATA = 5,     // Clamp 数据
    CONTROL = 10,       // 控制消息
    HEARTBEAT = 11      // 心跳
};

/**
 * @brief 通用消息头
 */
#pragma pack(push, 1)
struct MessageHeader {
    uint8_t type;           // MessageType
    uint64_t timestamp_ns;  // 时间戳（纳秒）
    uint32_t seq;           // 序列号
    uint32_t data_size;     // 数据大小
};

/**
 * @brief RGB 帧元数据
 */
struct RgbFrameMeta {
    uint32_t width;
    uint32_t height;
    uint32_t frame_index;
    double timestamp;      // 原始时间戳（秒）
    uint8_t encoding;      // 0=BGR, 1=RGB, 2=JPEG
};

/**
 * @brief 位姿数据（TUM 格式）
 */
struct PoseData {
    double timestamp;
    double x, y, z;
    double qx, qy, qz, qw;
};

/**
 * @brief Clamp 数据
 */
struct ClampData {
    double timestamp;
    double value;
};

/**
 * @brief 控制消息
 */
struct ControlMessage {
    uint8_t command;  // 0=STOP, 1=START, 2=PAUSE
    char device_serial[64];
    char output_dir[256];
};
#pragma pack(pop)

}  // namespace fast_data_collector

#endif  // FAST_DATA_COLLECTOR__ZMQ_PUBLISHER_HPP_

