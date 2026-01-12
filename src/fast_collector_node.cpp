/**
 * FastUMI C++ Data Collector Node
 * 
 * 高性能数据采集节点，使用 ZeroMQ 将数据传输到 Python 写盘进程
 */

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>
#include <xv_ros2_msgs/msg/clamp.hpp>

#include <chrono>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include <iostream>

#include "fast_data_collector/zmq_publisher.hpp"

// 使用 fast_data_collector 命名空间中的类型
using fast_data_collector::ZmqPublisher;
using fast_data_collector::MessageType;
using fast_data_collector::MessageHeader;
using fast_data_collector::RgbFrameMeta;
using fast_data_collector::PoseData;
using fast_data_collector::ClampData;
using fast_data_collector::ControlMessage;

namespace fast_data_collector {

/**
 * @brief 录制状态
 */
enum class RecordingState {
    IDLE,       // 空闲
    RECORDING,  // 录制中
    STOPPING    // 停止中
};

/**
 * @brief 高性能数据采集节点
 */
class FastCollectorNode : public rclcpp::Node {
public:
    explicit FastCollectorNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions())
        : Node("fast_data_collector", options)
        , state_(RecordingState::IDLE)
        , rgb_frame_index_(0)
        , slam_seq_(0)
        , vive_seq_(0)
        , tof_seq_(0)
        , clamp_seq_(0)
        , rgb_count_(0)
        , slam_count_(0)
        , vive_count_(0)
        , tof_count_(0)
        , clamp_count_(0)
        , rgb_fps_(0.0)
        , slam_fps_(0.0)
        , vive_fps_(0.0)
    {
        RCLCPP_INFO(this->get_logger(), "Initializing FastCollectorNode...");
        
        init_parameters();
        init_zmq_publishers();
        init_subscribers();
        
        // 预分配缓冲区
        jpeg_buffer_.reserve(1024 * 1024);  // 1MB
        send_buffer_.reserve(1024 * 1024 * 2);  // 2MB
        
        RCLCPP_INFO(this->get_logger(), "FastCollectorNode initialized successfully");
        RCLCPP_INFO(this->get_logger(), "  XV Serial: %s", xv_serial_.c_str());
        RCLCPP_INFO(this->get_logger(), "  Vive Serial: %s", vive_serial_.c_str());
        RCLCPP_INFO(this->get_logger(), "  ToF: %s", enable_tof_ ? "enabled" : "disabled");
        RCLCPP_INFO(this->get_logger(), "  JPEG Compression: %s (quality=%d)", 
                    use_jpeg_compression_ ? "enabled" : "disabled", jpeg_quality_);
    }
    
    ~FastCollectorNode() {
        stop_recording();
        RCLCPP_INFO(this->get_logger(), "FastCollectorNode destroyed");
    }
    
    void start_recording() {
        if (state_.load() == RecordingState::RECORDING) {
            RCLCPP_WARN(this->get_logger(), "Already recording!");
            return;
        }
        
        RCLCPP_INFO(this->get_logger(), "Starting recording...");
        
        // 重置计数器
        rgb_frame_index_ = 0;
        slam_seq_ = 0;
        vive_seq_ = 0;
        tof_seq_ = 0;
        clamp_seq_ = 0;
        rgb_count_ = 0;
        slam_count_ = 0;
        vive_count_ = 0;
        tof_count_ = 0;
        clamp_count_ = 0;
        
        // 发送开始控制消息
        send_control_message(1);  // START
        
        state_ = RecordingState::RECORDING;
        RCLCPP_INFO(this->get_logger(), "Recording started (max_rgb=%d)", max_rgb_count_);
    }
    
    void stop_recording() {
        if (state_.load() != RecordingState::RECORDING) {
            return;
        }
        
        RCLCPP_INFO(this->get_logger(), "Stopping recording...");
        state_ = RecordingState::STOPPING;
        
        // 发送停止控制消息
        send_control_message(0);  // STOP
        
        state_ = RecordingState::IDLE;
        
        RCLCPP_INFO(this->get_logger(), "Recording stopped. Statistics:");
        RCLCPP_INFO(this->get_logger(), "  RGB frames: %lu", rgb_count_.load());
        RCLCPP_INFO(this->get_logger(), "  SLAM poses: %lu", slam_count_.load());
        RCLCPP_INFO(this->get_logger(), "  Vive poses: %lu", vive_count_.load());
        RCLCPP_INFO(this->get_logger(), "  ToF frames: %lu", tof_count_.load());
        RCLCPP_INFO(this->get_logger(), "  Clamp data: %lu", clamp_count_.load());
    }
    
    RecordingState get_state() const { return state_.load(); }

private:
    void init_parameters() {
        // 声明并获取参数
        this->declare_parameter<std::string>("xv_serial", "");
        this->declare_parameter<std::string>("vive_serial", "");
        this->declare_parameter<std::string>("device_label", "");
        this->declare_parameter<std::string>("output_dir", "");
        this->declare_parameter<bool>("enable_tof", true);
        this->declare_parameter<bool>("enable_vive", true);
        this->declare_parameter<bool>("use_jpeg_compression", true);
        this->declare_parameter<int>("jpeg_quality", 95);
        this->declare_parameter<int>("max_rgb_count", 1800);
        
        // ZMQ 端点参数
        this->declare_parameter<std::string>("zmq_rgb_endpoint", "ipc:///tmp/fastumi_rgb");
        this->declare_parameter<std::string>("zmq_pose_endpoint", "ipc:///tmp/fastumi_pose");
        this->declare_parameter<std::string>("zmq_tof_endpoint", "ipc:///tmp/fastumi_tof");
        this->declare_parameter<std::string>("zmq_control_endpoint", "ipc:///tmp/fastumi_control");
        
        // 获取参数值
        xv_serial_ = this->get_parameter("xv_serial").as_string();
        vive_serial_ = this->get_parameter("vive_serial").as_string();
        device_label_ = this->get_parameter("device_label").as_string();
        output_dir_ = this->get_parameter("output_dir").as_string();
        enable_tof_ = this->get_parameter("enable_tof").as_bool();
        enable_vive_ = this->get_parameter("enable_vive").as_bool();
        use_jpeg_compression_ = this->get_parameter("use_jpeg_compression").as_bool();
        jpeg_quality_ = this->get_parameter("jpeg_quality").as_int();
        max_rgb_count_ = this->get_parameter("max_rgb_count").as_int();
        
        zmq_rgb_endpoint_ = this->get_parameter("zmq_rgb_endpoint").as_string();
        zmq_pose_endpoint_ = this->get_parameter("zmq_pose_endpoint").as_string();
        zmq_tof_endpoint_ = this->get_parameter("zmq_tof_endpoint").as_string();
        zmq_control_endpoint_ = this->get_parameter("zmq_control_endpoint").as_string();
        
        // 验证必要参数
        if (xv_serial_.empty()) {
            RCLCPP_ERROR(this->get_logger(), "xv_serial parameter is required!");
            throw std::runtime_error("xv_serial parameter is required");
        }
        
        // 检查 Vive 是否启用
        if (vive_serial_.empty() || vive_serial_ == "UNKNOWN") {
            enable_vive_ = false;
            RCLCPP_WARN(this->get_logger(), "Vive disabled (no valid serial)");
        }
    }

    void init_zmq_publishers() {
        RCLCPP_INFO(this->get_logger(), "Initializing ZMQ publishers...");
        
        try {
            rgb_publisher_ = std::make_unique<ZmqPublisher>(zmq_rgb_endpoint_);
            pose_publisher_ = std::make_unique<ZmqPublisher>(zmq_pose_endpoint_);
            control_publisher_ = std::make_unique<ZmqPublisher>(zmq_control_endpoint_);
            
            if (enable_tof_) {
                tof_publisher_ = std::make_unique<ZmqPublisher>(zmq_tof_endpoint_);
            }
            
            RCLCPP_INFO(this->get_logger(), "ZMQ publishers initialized");
        } catch (const std::exception& e) {
            RCLCPP_ERROR(this->get_logger(), "Failed to initialize ZMQ: %s", e.what());
            throw;
        }
    }

    void init_subscribers() {
        RCLCPP_INFO(this->get_logger(), "Initializing ROS2 subscribers...");
        
        // QoS 配置
        auto qos_rgb = rclcpp::QoS(rclcpp::KeepLast(1))
            .reliability(rclcpp::ReliabilityPolicy::Reliable);
        
        auto qos_pose = rclcpp::QoS(rclcpp::KeepLast(100))
            .reliability(rclcpp::ReliabilityPolicy::Reliable);
        
        auto qos_tof = rclcpp::QoS(rclcpp::KeepLast(5))
            .reliability(rclcpp::ReliabilityPolicy::Reliable);
        
        // 构建话题名称
        std::string rgb_topic = "/xv_sdk/" + xv_serial_ + "/rgb/image";
        std::string slam_topic = "/xv_sdk/" + xv_serial_ + "/pose";
        std::string tof_topic = "/xv_sdk/" + xv_serial_ + "/rgbPointCloud";
        std::string clamp_topic = "/xv_sdk/" + xv_serial_ + "/clamp";
        
        RCLCPP_INFO(this->get_logger(), "  RGB topic: %s", rgb_topic.c_str());
        RCLCPP_INFO(this->get_logger(), "  SLAM topic: %s", slam_topic.c_str());
        
        // 创建订阅器
        rgb_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
            rgb_topic, qos_rgb,
            std::bind(&FastCollectorNode::rgb_callback, this, std::placeholders::_1)
        );
        
        slam_sub_ = this->create_subscription<geometry_msgs::msg::PoseStamped>(
            slam_topic, qos_pose,
            std::bind(&FastCollectorNode::slam_callback, this, std::placeholders::_1)
        );
        
        // Vive 订阅
        if (enable_vive_) {
            std::string vive_serial_safe = vive_serial_;
            std::replace(vive_serial_safe.begin(), vive_serial_safe.end(), '-', '_');
            std::string vive_topic = "/vive/" + vive_serial_safe + "/pose";
            RCLCPP_INFO(this->get_logger(), "  Vive topic: %s", vive_topic.c_str());
            
            vive_sub_ = this->create_subscription<geometry_msgs::msg::PoseStamped>(
                vive_topic, qos_pose,
                std::bind(&FastCollectorNode::vive_callback, this, std::placeholders::_1)
            );
        }
        
        // ToF 订阅
        if (enable_tof_) {
            RCLCPP_INFO(this->get_logger(), "  ToF topic: %s", tof_topic.c_str());
            tof_sub_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
                tof_topic, qos_tof,
                std::bind(&FastCollectorNode::tof_callback, this, std::placeholders::_1)
            );
        }
        
        // Clamp 订阅
        RCLCPP_INFO(this->get_logger(), "  Clamp topic: %s", clamp_topic.c_str());
        clamp_sub_ = this->create_subscription<xv_ros2_msgs::msg::Clamp>(
            clamp_topic, qos_rgb,
            std::bind(&FastCollectorNode::clamp_callback, this, std::placeholders::_1)
        );
        
        RCLCPP_INFO(this->get_logger(), "ROS2 subscribers initialized");
    }

    // 回调函数
    void rgb_callback(sensor_msgs::msg::Image::SharedPtr msg) {
        if (state_.load() != RecordingState::RECORDING) {
            return;
        }
        
        send_rgb_frame(msg);
        rgb_count_++;
        
        // 检查是否达到最大帧数
        if (max_rgb_count_ > 0 && rgb_count_.load() >= static_cast<uint64_t>(max_rgb_count_)) {
            RCLCPP_INFO(this->get_logger(), "Reached max RGB count (%d), stopping...", max_rgb_count_);
            stop_recording();
        }
        
        // 更新 FPS
        update_fps_stats(rgb_timestamps_, rgb_fps_);
    }

    void slam_callback(geometry_msgs::msg::PoseStamped::SharedPtr msg) {
        if (state_.load() != RecordingState::RECORDING) {
            return;
        }
        
        send_pose(msg, MessageType::SLAM_POSE);
        slam_count_++;
        
        update_fps_stats(slam_timestamps_, slam_fps_);
    }

    void vive_callback(geometry_msgs::msg::PoseStamped::SharedPtr msg) {
        if (state_.load() != RecordingState::RECORDING) {
            return;
        }
        
        send_pose(msg, MessageType::VIVE_POSE);
        vive_count_++;
        
        update_fps_stats(vive_timestamps_, vive_fps_);
    }

    void tof_callback(sensor_msgs::msg::PointCloud2::SharedPtr msg) {
        if (state_.load() != RecordingState::RECORDING || !enable_tof_) {
            return;
        }
        
        send_pointcloud(msg);
        tof_count_++;
    }

    void clamp_callback(xv_ros2_msgs::msg::Clamp::SharedPtr msg) {
        if (state_.load() != RecordingState::RECORDING) {
            return;
        }
        
        double timestamp = msg->header.stamp.sec + msg->header.stamp.nanosec * 1e-9;
        double value = msg->data;
        
        send_clamp_data(timestamp, value);
        clamp_count_++;
    }

    // 发送函数
    void send_rgb_frame(const sensor_msgs::msg::Image::SharedPtr& msg) {
        double timestamp = msg->header.stamp.sec + msg->header.stamp.nanosec * 1e-9;
        uint32_t frame_index = rgb_frame_index_++;
        
        RgbFrameMeta meta;
        meta.width = msg->width;
        meta.height = msg->height;
        meta.frame_index = frame_index;
        meta.timestamp = timestamp;
        
        const uint8_t* image_data = nullptr;
        size_t image_size = 0;
        
        if (use_jpeg_compression_) {
            try {
                cv_bridge::CvImageConstPtr cv_ptr = cv_bridge::toCvShare(msg, "bgr8");
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality_};
                cv::imencode(".jpg", cv_ptr->image, jpeg_buffer_, params);
                
                image_data = jpeg_buffer_.data();
                image_size = jpeg_buffer_.size();
                meta.encoding = 2;  // JPEG
            } catch (const cv_bridge::Exception& e) {
                RCLCPP_ERROR_THROTTLE(this->get_logger(), *this->get_clock(), 1000,
                                      "cv_bridge error: %s", e.what());
                return;
            }
        } else {
            image_data = msg->data.data();
            image_size = msg->data.size();
            meta.encoding = (msg->encoding == "bgr8") ? 0 : 1;
        }
        
        MessageHeader header;
        header.type = static_cast<uint8_t>(MessageType::RGB_FRAME);
        header.timestamp_ns = static_cast<uint64_t>(timestamp * 1e9);
        header.seq = frame_index;
        header.data_size = sizeof(RgbFrameMeta) + image_size;
        
        size_t total_size = sizeof(MessageHeader) + sizeof(RgbFrameMeta) + image_size;
        send_buffer_.resize(total_size);
        
        uint8_t* ptr = send_buffer_.data();
        std::memcpy(ptr, &header, sizeof(MessageHeader));
        ptr += sizeof(MessageHeader);
        std::memcpy(ptr, &meta, sizeof(RgbFrameMeta));
        ptr += sizeof(RgbFrameMeta);
        std::memcpy(ptr, image_data, image_size);
        
        if (!rgb_publisher_->send(send_buffer_)) {
            RCLCPP_WARN_THROTTLE(this->get_logger(), *this->get_clock(), 1000,
                                 "Failed to send RGB frame %u", frame_index);
        }
    }

    void send_pose(const geometry_msgs::msg::PoseStamped::SharedPtr& msg, MessageType type) {
        double timestamp = msg->header.stamp.sec + msg->header.stamp.nanosec * 1e-9;
        
        PoseData pose;
        pose.timestamp = timestamp;
        pose.x = msg->pose.position.x;
        pose.y = msg->pose.position.y;
        pose.z = msg->pose.position.z;
        pose.qx = msg->pose.orientation.x;
        pose.qy = msg->pose.orientation.y;
        pose.qz = msg->pose.orientation.z;
        pose.qw = msg->pose.orientation.w;
        
        MessageHeader header;
        header.type = static_cast<uint8_t>(type);
        header.timestamp_ns = static_cast<uint64_t>(timestamp * 1e9);
        header.seq = (type == MessageType::SLAM_POSE) ? slam_seq_++ : vive_seq_++;
        header.data_size = sizeof(PoseData);
        
        size_t total_size = sizeof(MessageHeader) + sizeof(PoseData);
        uint8_t buffer[sizeof(MessageHeader) + sizeof(PoseData)];
        
        std::memcpy(buffer, &header, sizeof(MessageHeader));
        std::memcpy(buffer + sizeof(MessageHeader), &pose, sizeof(PoseData));
        
        pose_publisher_->send(buffer, total_size);
    }

    void send_pointcloud(const sensor_msgs::msg::PointCloud2::SharedPtr& msg) {
        if (!tof_publisher_) return;
        
        double timestamp = msg->header.stamp.sec + msg->header.stamp.nanosec * 1e-9;
        
        MessageHeader header;
        header.type = static_cast<uint8_t>(MessageType::TOF_POINTCLOUD);
        header.timestamp_ns = static_cast<uint64_t>(timestamp * 1e9);
        header.seq = tof_seq_++;
        header.data_size = msg->data.size();
        
        size_t total_size = sizeof(MessageHeader) + msg->data.size();
        std::vector<uint8_t> buffer(total_size);
        
        std::memcpy(buffer.data(), &header, sizeof(MessageHeader));
        std::memcpy(buffer.data() + sizeof(MessageHeader), msg->data.data(), msg->data.size());
        
        tof_publisher_->send(buffer);
    }

    void send_clamp_data(double timestamp, double value) {
        ClampData clamp;
        clamp.timestamp = timestamp;
        clamp.value = value;
        
        MessageHeader header;
        header.type = static_cast<uint8_t>(MessageType::CLAMP_DATA);
        header.timestamp_ns = static_cast<uint64_t>(timestamp * 1e9);
        header.seq = clamp_seq_++;
        header.data_size = sizeof(ClampData);
        
        size_t total_size = sizeof(MessageHeader) + sizeof(ClampData);
        uint8_t buffer[sizeof(MessageHeader) + sizeof(ClampData)];
        
        std::memcpy(buffer, &header, sizeof(MessageHeader));
        std::memcpy(buffer + sizeof(MessageHeader), &clamp, sizeof(ClampData));
        
        pose_publisher_->send(buffer, total_size);
    }

    void send_control_message(uint8_t command) {
        ControlMessage ctrl;
        ctrl.command = command;
        std::strncpy(ctrl.device_serial, xv_serial_.c_str(), sizeof(ctrl.device_serial) - 1);
        ctrl.device_serial[sizeof(ctrl.device_serial) - 1] = '\0';
        std::strncpy(ctrl.output_dir, output_dir_.c_str(), sizeof(ctrl.output_dir) - 1);
        ctrl.output_dir[sizeof(ctrl.output_dir) - 1] = '\0';
        
        MessageHeader header;
        header.type = static_cast<uint8_t>(MessageType::CONTROL);
        header.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        header.seq = 0;
        header.data_size = sizeof(ControlMessage);
        
        size_t total_size = sizeof(MessageHeader) + sizeof(ControlMessage);
        uint8_t buffer[sizeof(MessageHeader) + sizeof(ControlMessage)];
        
        std::memcpy(buffer, &header, sizeof(MessageHeader));
        std::memcpy(buffer + sizeof(MessageHeader), &ctrl, sizeof(ControlMessage));
        
        control_publisher_->send(buffer, total_size, 0);
    }

    void update_fps_stats(std::deque<double>& timestamps, double& current_fps) {
        auto now = std::chrono::steady_clock::now();
        double now_sec = std::chrono::duration<double>(now.time_since_epoch()).count();
        
        std::lock_guard<std::mutex> lock(fps_mutex_);
        
        timestamps.push_back(now_sec);
        
        while (!timestamps.empty() && (now_sec - timestamps.front()) > 1.0) {
            timestamps.pop_front();
        }
        
        if (timestamps.size() >= 2) {
            double dt = timestamps.back() - timestamps.front();
            if (dt > 0) {
                current_fps = (timestamps.size() - 1) / dt;
            }
        }
    }

    // 成员变量
    std::string xv_serial_;
    std::string vive_serial_;
    std::string device_label_;
    std::string output_dir_;
    bool enable_tof_;
    bool enable_vive_;
    bool use_jpeg_compression_;
    int jpeg_quality_;
    int max_rgb_count_;
    
    std::string zmq_rgb_endpoint_;
    std::string zmq_pose_endpoint_;
    std::string zmq_tof_endpoint_;
    std::string zmq_control_endpoint_;
    
    std::unique_ptr<ZmqPublisher> rgb_publisher_;
    std::unique_ptr<ZmqPublisher> pose_publisher_;
    std::unique_ptr<ZmqPublisher> tof_publisher_;
    std::unique_ptr<ZmqPublisher> control_publisher_;
    
    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr rgb_sub_;
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr slam_sub_;
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr vive_sub_;
    rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr tof_sub_;
    rclcpp::Subscription<xv_ros2_msgs::msg::Clamp>::SharedPtr clamp_sub_;
    
    std::atomic<RecordingState> state_;
    std::atomic<uint32_t> rgb_frame_index_;
    std::atomic<uint32_t> slam_seq_;
    std::atomic<uint32_t> vive_seq_;
    std::atomic<uint32_t> tof_seq_;
    std::atomic<uint32_t> clamp_seq_;
    
    std::atomic<uint64_t> rgb_count_;
    std::atomic<uint64_t> slam_count_;
    std::atomic<uint64_t> vive_count_;
    std::atomic<uint64_t> tof_count_;
    std::atomic<uint64_t> clamp_count_;
    
    std::deque<double> rgb_timestamps_;
    std::deque<double> slam_timestamps_;
    std::deque<double> vive_timestamps_;
    double rgb_fps_;
    double slam_fps_;
    double vive_fps_;
    std::mutex fps_mutex_;
    
    std::vector<uint8_t> jpeg_buffer_;
    std::vector<uint8_t> send_buffer_;
};

}  // namespace fast_data_collector

// 主函数
int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    
    auto node = std::make_shared<fast_data_collector::FastCollectorNode>();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "FastUMI C++ Data Collector" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Press ENTER to start recording..." << std::endl;
    std::cin.get();
    
    node->start_recording();
    
    rclcpp::executors::MultiThreadedExecutor executor;
    executor.add_node(node);
    
    std::cout << "Recording... Press Ctrl+C to stop." << std::endl;
    
    try {
        executor.spin();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    node->stop_recording();
    rclcpp::shutdown();
    
    return 0;
}
