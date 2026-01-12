"""
FastUMI C++ Data Collector 启动文件
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        # 声明参数
        DeclareLaunchArgument('xv_serial', description='XV SDK device serial number'),
        DeclareLaunchArgument('vive_serial', default_value='', description='Vive tracker serial number'),
        DeclareLaunchArgument('device_label', default_value='', description='Device label (e.g., left, right)'),
        DeclareLaunchArgument('output_dir', default_value='', description='Output directory'),
        DeclareLaunchArgument('enable_tof', default_value='true', description='Enable ToF point cloud'),
        DeclareLaunchArgument('enable_vive', default_value='true', description='Enable Vive tracking'),
        DeclareLaunchArgument('use_jpeg_compression', default_value='true', description='Use JPEG compression for RGB'),
        DeclareLaunchArgument('jpeg_quality', default_value='95', description='JPEG quality (1-100)'),
        DeclareLaunchArgument('max_rgb_count', default_value='1800', description='Max RGB frames before auto stop'),
        
        # ZMQ 端点参数
        DeclareLaunchArgument('zmq_rgb_endpoint', default_value='ipc:///tmp/fastumi_rgb'),
        DeclareLaunchArgument('zmq_pose_endpoint', default_value='ipc:///tmp/fastumi_pose'),
        DeclareLaunchArgument('zmq_tof_endpoint', default_value='ipc:///tmp/fastumi_tof'),
        DeclareLaunchArgument('zmq_control_endpoint', default_value='ipc:///tmp/fastumi_control'),
        
        # 启动节点
        Node(
            package='fast_data_collector',
            executable='fast_collector_node',
            name='fast_data_collector',
            output='screen',
            parameters=[{
                'xv_serial': LaunchConfiguration('xv_serial'),
                'vive_serial': LaunchConfiguration('vive_serial'),
                'device_label': LaunchConfiguration('device_label'),
                'output_dir': LaunchConfiguration('output_dir'),
                'enable_tof': LaunchConfiguration('enable_tof'),
                'enable_vive': LaunchConfiguration('enable_vive'),
                'use_jpeg_compression': LaunchConfiguration('use_jpeg_compression'),
                'jpeg_quality': LaunchConfiguration('jpeg_quality'),
                'max_rgb_count': LaunchConfiguration('max_rgb_count'),
                'zmq_rgb_endpoint': LaunchConfiguration('zmq_rgb_endpoint'),
                'zmq_pose_endpoint': LaunchConfiguration('zmq_pose_endpoint'),
                'zmq_tof_endpoint': LaunchConfiguration('zmq_tof_endpoint'),
                'zmq_control_endpoint': LaunchConfiguration('zmq_control_endpoint'),
            }]
        ),
    ])

