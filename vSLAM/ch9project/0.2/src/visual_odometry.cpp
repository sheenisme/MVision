/*
 *myslam::VisualOdometry
 * ORB特征点 描述子 汉明字符串距离匹配得到 特征点对
 * 其中 一幅图像的2D像素点 根据 深度信息和 相机内参数 转化成 3D点，组成2D-3D点对
 * 使用随机采样序列的 PnP算法求解 转换矩阵 根据 符合 转换矩阵的内点数量判断 求解得到的 转换矩阵的好坏
 * 把第一帧图像设为  世界坐标系原点
 * 以后的每一帧通过 与上一帧的转换矩阵 转换到世界坐标系下
 * 根据得到的 转换矩阵，若 旋转向量 和 平移矩阵 的大小都超过一定限度，则认为该帧是一个关键帧
 * 保存3D坐标 和 对应的描述子
 */

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <algorithm>
#include <boost/timer.hpp>

#include "myslam/config.h"
#include "myslam/visual_odometry.h"
/*视觉里程记*/
namespace myslam//命令空间下 防止定义的出其他库里的同名函数
{
    //【1】默认默认构造函数
    VisualOdometry::VisualOdometry() :   //视觉里程计类
	state_ ( INITIALIZING ), ref_ ( nullptr ), curr_ ( nullptr ), map_ ( new Map ), num_lost_ ( 0 ), num_inliers_ ( 0 )
    {
	num_of_features_    = Config::get<int> ( "number_of_features" );// 特征数量   整型 int
	scale_factor_       = Config::get<double> ( "scale_factor" );            // 尺度因子 缩小
	level_pyramid_      = Config::get<int> ( "level_pyramid" );              // 层级 
	match_ratio_        = Config::get<float> ( "match_ratio" );               // 匹配 参数
	max_num_lost_       = Config::get<float> ( "max_num_lost" );       // 最大丢失次数
	min_inliers_        = Config::get<int> ( "min_inliers" );       		// 最小内点数量
	key_frame_min_rot   = Config::get<double> ( "keyframe_rotation" );//最小的旋转      成为关键帧的条件
	key_frame_min_trans = Config::get<double> ( "keyframe_translation" ); // 最小平移  
	orb_ = cv::ORB::create ( num_of_features_, scale_factor_, level_pyramid_ );//创建orb特征
    }
    // 【2】析构函数   shared_ptr 智能指针 自带析构函数功能
    VisualOdometry::~VisualOdometry(){}
    // 添加新的一帧 add a new frame 
    bool VisualOdometry::addFrame ( Frame::Ptr frame )
    {
	switch ( state_ )
	{
	      case INITIALIZING://初始化之后
	      {
		  state_ = OK;//初始化之后切换状态为 OK
		  curr_ = ref_ = frame;//初始化 参考帧 当前帧
		  map_->insertKeyFrame ( frame );//添加第一个关键帧 到地图内
		  // extract features from first frame 
		  extractKeyPoints();//提取关键点
		  computeDescriptors();//计算描述子
		  setRef3DPoints();//计算特征点的 3维坐标
		  break;
	      }
	      case OK://初始化之后
	      {
		  curr_ = frame;//新的一帧为当前帧
		  extractKeyPoints();//提取关键点
		  computeDescriptors();//计算描述子
		  featureMatching();      //特征点匹配 得到 特征点匹配点对
		  poseEstimationPnP(); //根据特征点匹配点对  计算坐标转换 及估计相机位姿
		  if ( checkEstimatedPose() == true ) // a good estimation  估计的效果理想
		  {
		      curr_->T_c_w_ = T_c_r_estimated_ * ref_->T_c_w_;  // T_c_w = T_c_r * T_r_w  // 当前帧的转换*参考帧的转换 ——> 起始帧到当前帧的转换
		      ref_ = curr_;//迭代当前帧成参考帧
		      setRef3DPoints();//计算特征点的 3维坐标
		      num_lost_ = 0;//重置 丢失计数
		      if ( checkKeyFrame() == true ) // is a key-frame 是关键帧吗?
		      {
			  addKeyFrame();//是关键帧的话 加入关键帧
		      }
		  }
		  else // 估计的效果不理想bad estimation due to various reasons
		  {
		      num_lost_++;//认为跟丢了
		      if ( num_lost_ > max_num_lost_ )//如果连续跟丢失次数 大于设置的限制
		      {
			  state_ = LOST;//设置状态为丢失
		      }
		      return false;//并返回错误
		  }
		  break;
	      }
	      case LOST://在状态为 丢失的情况下
	      {
		  cout<<"视觉里程计丢失 vo has lost."<<endl;//打印丢失
		  break;//直接退出
	      }
	      }

	return true;
    }

    //检测关键点 orb特征点检测
    void VisualOdometry::extractKeyPoints()
    {
	orb_->detect ( curr_->color_, keypoints_curr_ );//对当前彩色图 提取orb关键点 存放在 keypoints_curr_ 当前帧 关键点
    }
    //计算关键点对应的描述子 
    void VisualOdometry::computeDescriptors()
    {
	orb_->compute ( curr_->color_, keypoints_curr_, descriptors_curr_ );
	//输入 彩色图 关键点 得到的描述子 存放在 descriptors_curr_ 关键点对应的orb描述子
    }
    //特征匹配
    void VisualOdometry::featureMatching()
    {
	// match desp_ref and desp_curr, use OpenCV's brute force match 
	vector<cv::DMatch> matches;//匹配结果
	cv::BFMatcher matcher ( cv::NORM_HAMMING );//汉明匹配器  二进制字符串匹配 距离
	matcher.match ( descriptors_ref_, descriptors_curr_, matches );//根据描述子进行匹配
	//前后两帧 的特征点描述子 进行汉明长度 匹配  匹配结果存放在 matches中(各个描述子间的距离)
	// select the best matches 选择较好的特征点描述子匹配对
	
	//计算最小的匹配距离  筛选较好的匹配点对
	float min_dis = std::min_element (
			    matches.begin(), matches.end(),
			    [] ( const cv::DMatch& m1, const cv::DMatch& m2 )
	{
	    return m1.distance < m2.distance;
	} )->distance;
      
	feature_matches_.clear();
	for ( cv::DMatch& m : matches )
	{
	    if ( m.distance < max<float> ( min_dis*match_ratio_, 30.0 ) )//最小距离限制 30
	    {
		feature_matches_.push_back(m);//较小的匹配距离都是 好的匹配 特征点对 即在两幅图像中表示 世界坐标系中的同一个点
	    }
	}
	cout<<"good matches: "<<feature_matches_.size()<<endl;//好的匹配对数量
    }

    //设置参考点 三维坐标
    // PnP算法所需要的 3D点
    void VisualOdometry::setRef3DPoints()
    {
	// select the features with depth measurements 
	pts_3d_ref_.clear();//清除上一次的点
	descriptors_ref_ = Mat();// 关键点对应的描述子 和 对应的3D点
	for ( size_t i=0; i<keypoints_curr_.size(); i++ )
	{
	    double d = ref_->findDepth(keypoints_curr_[i]);//关键点对应的深度               
	    if ( d > 0)
	    {
		Vector3d p_cam = ref_->camera_->pixel2camera(//像素二维坐标转换到相机三维坐标
		    Vector2d(keypoints_curr_[i].pt.x, keypoints_curr_[i].pt.y), d
		);
		pts_3d_ref_.push_back( cv::Point3f( p_cam(0,0), p_cam(1,0), p_cam(2,0) ));//像极坐标系下的 三维坐标
		descriptors_ref_.push_back(descriptors_curr_.row(i));					//对应的描述子
	    }
	}
    }

    // 2D-3D 匹配点 得到  转换矩阵
    void VisualOdometry::poseEstimationPnP()
    {
	// construct the 3d 2d observations
	vector<cv::Point3f> pts3d;
	vector<cv::Point2f> pts2d;
	
	for ( cv::DMatch m:feature_matches_ )
	{
	    pts3d.push_back( pts_3d_ref_[m.queryIdx] );
	    pts2d.push_back( keypoints_curr_[m.trainIdx].pt );
	}
	//相机内参数
	Mat K = ( cv::Mat_<double>(3,3)<<
	    ref_->camera_->fx_, 0, ref_->camera_->cx_,
	    0, ref_->camera_->fy_, ref_->camera_->cy_,
	    0,0,1
	);
	// PnP算法求解  2D-3D点对求解 旋转向量 rvec,  平移矩阵 tvec
	Mat rvec, tvec, inliers;
	cv::solvePnPRansac( pts3d, pts2d, K, Mat(), rvec, tvec, false, 100, 4.0, 0.99, inliers );// 随机采样序列 算法  得到符合求解得到的R t 的点的数量
	num_inliers_ = inliers.rows;//内点数量
	cout<<"pnp inliers: "<<num_inliers_<<endl;
	T_c_r_estimated_ = SE3(
	    SO3(rvec.at<double>(0,0), rvec.at<double>(1,0), rvec.at<double>(2,0)), //旋转向量
	    Vector3d( tvec.at<double>(0,0), tvec.at<double>(1,0), tvec.at<double>(2,0))//平移矩阵
	);
    }

    //核对估计的位姿
    bool VisualOdometry::checkEstimatedPose()
    {
	// check if the estimated pose is good
	if ( num_inliers_ < min_inliers_ )
	{
	    cout<<"reject because inlier is too small: "<<num_inliers_<<endl;
	    return false;
	}
	// if the motion is too large, it is probably wrong
	Sophus::Vector6d d = T_c_r_estimated_.log();//转换矩阵 李代数形式
	if ( d.norm() > 5.0 )//动静太大了，不可能
	{
	    cout<<"运动量过大，求解有误 reject because motion is too large: "<<d.norm()<<endl;
	    return false;
	}
	return true;
    }

    //检查是否为关键帧   旋转和平移都达到一定的量 则认为 是 关键帧
    bool VisualOdometry::checkKeyFrame()
    {
	Sophus::Vector6d d = T_c_r_estimated_.log();//转换矩阵 李代数形式
	Vector3d trans = d.head<3>();//平移向量
	Vector3d rot = d.tail<3>();// 旋转向量
	if ( rot.norm() >key_frame_min_rot || trans.norm() >key_frame_min_trans )
	    return true;
	return false;
    }

    //添加关键帧
    void VisualOdometry::addKeyFrame()
    {
	cout<<"添加关键帧  adding a key-frame"<<endl;
	map_->insertKeyFrame ( curr_ );
    }

}
