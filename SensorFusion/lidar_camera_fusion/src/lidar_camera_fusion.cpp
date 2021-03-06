/*
 * @Description: ROS Node, Fusion img and point cloud
 * @Author: Dlonng
 * @Date: 2020-05-03 20:47:00
 * @LastEditTime: 
 */


#include "lidar_camera_fusion.h"


LidarCameraFusion::LidarCameraFusion() 
    : camera_lidar_tf_ok(false)
    , image_frame_id("") {

    InitROS();
}


/*
 * @Description: 初始化当前节点，获取参数，订阅话题，发布话题
 * @Author: Dlonng
 * @Date: 2020-05-04 
 * @LastEditTime: 
 */
void LidarCameraFusion::InitROS() {
    std::string image_input;
    std::string cloud_input;
    std::string fusion_topic;
    
    // 获取的参数名：image_input，获取的参数值存储在：image_input，缺省值：/cv_camera/image_raw
    param_handle.param<std::string>("image_input", image_input, "/cv_camera/image_raw");
    param_handle.param<std::string>("cloud_input", cloud_input, "/velodyne_points");
    param_handle.param<std::string>("fusion_topic", fusion_topic, "/fusion_cloud");

    // 订阅 image_input 话题
    // 第二个参数是队列大小，以防我们处理消息的速度不够快，当缓存达到 1 条消息后，再有新的消息到来就将开始丢弃先前接收的消息。
    // 当处理消息速度不够时，可以调大第二个参数！
    // 收到订阅消息后调用 ImageCallback 处理图像 msg
    sub_image = topic_handle.subscribe(image_input, 1, &LidarCameraFusion::ImageCallback, this);

    sub_cloud = topic_handle.subscribe(cloud_input, 1, &LidarCameraFusion::CloudCallback, this);
    
    // 在 fusion_topic 上发布 sensor_msgs::PointCloud2 类型的消息
    // 第二个参数为缓冲区大小，缓冲区被占满后会丢弃先前发布的消息，目前为 1，可能需要更改！
    // 发布消息：pub_fusion_cloud.publish(msg)
    pub_fusion_cloud = topic_handle.advertise<sensor_msgs::PointCloud2>(fusion_topic, 1);
}


/*
 * @Description: 使用相机内参矩阵，畸变矩阵和 OpenCV 对图像去畸变，并发布矫正后的图像消息
 * @Author: Dlonng
 * @Date: 2020-05-04 
 * @LastEditTime: 
 */
void LidarCameraFusion::ImageCallback(const sensor_msgs::Image::ConstPtr& image_msg) {
    // 1. 确保相机内参和畸变矩阵已经初始化！

    // 2. ros_img -> cv_imag 
    // image_msg: 图像指针，brg8: 编码参数
    // rgb8: CV_8UC3, color image with red-green-blue color order
    // https://blog.csdn.net/bigdog_1027/article/details/79090571
    cv_bridge::CvImagePtr cv_image_ptr = cv_bridge::toCvCopy(image_msg, "brg8");
    cv::Mat cv_image = cv_image_ptr->image;
    

    // 3. OpenCV 去畸变
    // cv_image: 原畸变 OpenCV 图像，image_frame：去畸变后的图像
    // camera_instrinsics：相机内参矩阵，distortion_coefficients：相机畸变矩阵
    cv::undistort(cv_image, image_frame, camera_instrinsics, distortion_coefficients);

    // 4. 发布去畸变的图像消息

    // 5. 保存当前图像帧的 ID 和大小
    image_frame_id = image_msg->header.frame_id;
    image_size.height = image_frame.rows;
    image_size.width = image_frame.cols;
}

/*
 * @Description: 融合点云和图像
 * @Author: Dlonng
 * @Date: 2020-05-04 
 * @LastEditTime: 
 */
void LidarCameraFusion::CloudCallback(const sensor_msgs::PointCloud2::ConstPtr& cloud_msg) {
    // 1. 确保当前融合的图像已经去畸变
    if (current_frame.empty() || image_frame_id = "") {
        ROS_INFO("lidar_camera_fusion: waiting for current image frame ...");
        return ;
    }

    // 2. 从 tf 树中寻找雷达和相机的坐标变换关系
    if (camera_lidar_tf_ok == false)
        camera_lidar_tf = FindTransform(image_frame_id, cloud_msg->header.frame_id);

    // 3. 保证相机内参和坐标转换准备完成
    if (/* 相机内参 == false */ || camera_lidar_tf_ok == false) {
        ROS_INFO("lidar_camera_fusion: waiting for camera intrinsics and camera lidar tf ...");
        return ;
    }

    // 4. ROS point cloud -> PCL point cloud
    pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud_msg(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::fromROSMsg(*cloud_msg, *in_cloud_msg);

    // 5. 需不需要对 PCL 点云做分割地面等操作？
    
    // 融合后的点云
    pcl::PointCloud<pcl::PointXYZRGB>::ptr out_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);

    out_cloud->points.clear();

    pcl::PointXYZRGB color_point;

    std::vector<pcl::PointXYZ> cam_cloud(in_cloud_msg->points.size());
    
    int row = 0;
    int col = 0;
    cv::Vec3b rgb_pixel;

    // 6. 遍历点云，给每个点加上颜色
    for (size_t i = 0; i < in_cloud_msg->points.size(); i++) {
        // 或者以下的坐标转换直接用矩阵相乘
        cam_cloud[i] = TransformPoint(in_cloud_msg->points[i], camera_lidar_tf);

        // 使用相机内参将三维空间点投影到像素平面
        row = int(cam_cloud[i].y * fy / cam_cloud[i].z + cy);
        col = int(cam_cloud[i].x * fx / cam_cloud[i].z + cx);

        if ((row >= 0) && (row < image_size.height) && (col >= 0) && (col < image_size.width) && cam_cloud[i].z > 0) {
            color_point.x = in_cloud->points[i].x;
            color_point.y = in_cloud->points[i].y;
            color_point.z = in_cloud->points[i].z;

            rgb_pixel = image_frame.at<cv::Vec3b>(row, col);

            color_point.r = rgb_pixel[2];
            color_point.g = rgb_pixel[1];
            color_point.b = rgb_pixel[0];

            out_cloud->points.push_back(color_point);
        }
    }

    sensor_msgs::PointCloud2 fusion_cloud;

    // PCL cloud -> ROS cloud
    pcl::toROSMsg(*out_cloud, fusion_cloud);

    fusion_cloud.header = cloud_msg->header;

    // 发布融合后的带颜色的点云
    pub_fusion_cloud.publish(fusion_cloud);
}


/*
 * @Description: 在 TF 树中寻找雷达和相机的坐标转换关系并返回
 * @Author: Dlonng
 * @Date: 2020-05-05
 * @LastEditTime: 
 */
tf::StampedTransform LidarCameraFusion::FindTransform(const std::string& target_frame, const std::string source_frame) {
    tf::StampedTransform transform;

    camera_lidar_tf_ok = false;

    try {
        transform_listener.lookupTransform(target_frame, source_frame, ros::Time(0), transform);
        camera_lidar_tf_ok = true;
        ROS_INFO("lidar_camera_fusion: camera_lidar_tf Get!");
    } catch(tf::TransformException e) {
        ROS_INFO("lidar_camera_fusion: %s", e.what());
    }
    
    return transform;
}

/*
 * @Description: 对输入点云做 transform 坐标变换
 * @Author: Dlonng
 * @Date: 2020-05-05
 * @LastEditTime: 
 */
pcl::PointXYZ LidarCameraFusion::TransformPoint(const pcl::PointXYZ& in_point, const tf::StampedTransform& in_transform) {
    tf::Vector3 point(in_point.x, in_point.y, in_point.z);

    tf::Vector3 point_transform = in_transform * point;

    return pcl::PointXYZ(point_transform.x(), point_transform.y(), point_transform.z());
}

