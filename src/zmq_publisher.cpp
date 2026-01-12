#include "fast_data_collector/zmq_publisher.hpp"
#include <iostream>
#include <stdexcept>

namespace fast_data_collector {

ZmqPublisher::ZmqPublisher(const std::string& endpoint)
    : context_(nullptr)
    , socket_(nullptr)
    , endpoint_(endpoint)
    , connected_(false)
    , sent_count_(0)
    , failed_count_(0)
{
    // 创建 ZMQ 上下文
    context_ = zmq_ctx_new();
    if (!context_) {
        throw std::runtime_error("Failed to create ZMQ context");
    }
    
    // 创建 PUSH socket（用于负载均衡的管道模式）
    socket_ = zmq_socket(context_, ZMQ_PUSH);
    if (!socket_) {
        zmq_ctx_destroy(context_);
        throw std::runtime_error("Failed to create ZMQ socket");
    }
    
    // 设置 socket 选项
    int linger = 0;  // 关闭时不等待
    zmq_setsockopt(socket_, ZMQ_LINGER, &linger, sizeof(linger));
    
    int sndhwm = 100;  // 发送高水位标记
    zmq_setsockopt(socket_, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
    
    int sndbuf = 1024 * 1024 * 64;  // 64MB 发送缓冲区
    zmq_setsockopt(socket_, ZMQ_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    // 绑定到端点
    int rc = zmq_bind(socket_, endpoint_.c_str());
    if (rc != 0) {
        zmq_close(socket_);
        zmq_ctx_destroy(context_);
        throw std::runtime_error("Failed to bind ZMQ socket to " + endpoint_ + 
                                 ": " + zmq_strerror(zmq_errno()));
    }
    
    connected_ = true;
    std::cout << "[ZmqPublisher] Bound to " << endpoint_ << std::endl;
}

ZmqPublisher::~ZmqPublisher() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (socket_) {
        zmq_close(socket_);
        socket_ = nullptr;
    }
    
    if (context_) {
        zmq_ctx_destroy(context_);
        context_ = nullptr;
    }
    
    connected_ = false;
    std::cout << "[ZmqPublisher] Closed. Sent: " << sent_count_ 
              << ", Failed: " << failed_count_ << std::endl;
}

bool ZmqPublisher::send(const void* data, size_t size, int flags) {
    if (!connected_ || !socket_) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    zmq_msg_t msg;
    zmq_msg_init_size(&msg, size);
    memcpy(zmq_msg_data(&msg), data, size);
    
    int rc = zmq_msg_send(&msg, socket_, flags);
    zmq_msg_close(&msg);
    
    if (rc == -1) {
        int err = zmq_errno();
        if (err != EAGAIN) {  // EAGAIN 是非阻塞模式下的正常情况
            failed_count_++;
        }
        return false;
    }
    
    sent_count_++;
    return true;
}

bool ZmqPublisher::send(const std::vector<uint8_t>& data, int flags) {
    return send(data.data(), data.size(), flags);
}

bool ZmqPublisher::send(const std::string& data, int flags) {
    return send(data.data(), data.size(), flags);
}

}  // namespace fast_data_collector

