cmake_minimum_required(VERSION 3.8)
project(fast_data_collector)

# 编译选项
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -O3)
endif()

# 查找依赖
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(image_transport REQUIRED)
find_package(xv_ros2_msgs REQUIRED)
find_package(OpenCV REQUIRED)

# 查找 ZeroMQ
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

# 包含目录
include_directories(
  include
  ${OpenCV_INCLUDE_DIRS}
  ${ZMQ_INCLUDE_DIRS}
)

# 主节点可执行文件
add_executable(fast_collector_node
  src/fast_collector_node.cpp
  src/zmq_publisher.cpp
)

target_link_libraries(fast_collector_node
  ${OpenCV_LIBRARIES}
  ${ZMQ_LIBRARIES}
)

ament_target_dependencies(fast_collector_node
  rclcpp
  sensor_msgs
  geometry_msgs
  cv_bridge
  image_transport
  xv_ros2_msgs
)

# 安装目标
install(TARGETS
  fast_collector_node
  DESTINATION lib/${PROJECT_NAME}
)

# 安装配置和启动文件
install(DIRECTORY
  launch
  config
  DESTINATION share/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()

