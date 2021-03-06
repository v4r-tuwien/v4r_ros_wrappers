#include "recognizer_ros.h"
#include <pcl_conversions/pcl_conversions.h>
#include <cv_bridge/cv_bridge.h>
#include <v4r/common/pcl_opencv.h>
#include <v4r/reconstruction/impl/projectPointToImage.hpp>

#include <iostream>
#include <sstream>

namespace po = boost::program_options;

namespace v4r
{
template<typename PointT>
bool
RecognizerROS<PointT>::respondSrvCall(v4r_object_recognition_msgs::recognize::Request &req,
                                      v4r_object_recognition_msgs::recognize::Response &response) const
{
    typename pcl::PointCloud<PointT>::Ptr pRecognizedModels (new pcl::PointCloud<PointT>);

    // convert point cloud
    v4r::PCLOpenCVConverter<PointT> img_conv;
    img_conv.setInputCloud(scene_);
    img_conv.setCamera( camera_ );
    cv::Mat annotated_img = img_conv.getRGBImage();

    float intrinsic[9] = { camera_->getFocalLengthX(), 0, camera_->getCx(), 0, camera_->getFocalLengthY(), camera_->getCy(), 0.f, 0.f, 1.f};

    for(size_t ohg_id=0; ohg_id<object_hypotheses_.size(); ohg_id++)
    {
        for(const v4r::ObjectHypothesis::Ptr &oh : object_hypotheses_[ohg_id].ohs_)
        {
            if( ! oh->is_verified_ )
                continue;

            std_msgs::String ss_tmp;
            ss_tmp.data = oh->model_id_;
            response.ids.push_back(ss_tmp);

            Eigen::Matrix4f trans = oh->pose_refinement_ * oh->transform_;
            geometry_msgs::Transform tt;
            tt.translation.x = trans(0,3);
            tt.translation.y = trans(1,3);
            tt.translation.z = trans(2,3);

            Eigen::Matrix3f rotation = trans.block<3,3>(0,0);
            Eigen::Quaternionf q(rotation);
            tt.rotation.x = q.x();
            tt.rotation.y = q.y();
            tt.rotation.z = q.z();
            tt.rotation.w = q.w();
            response.transforms.push_back(tt);

            typename pcl::PointCloud<PointT>::ConstPtr model_cloud = mrec_->getModel( oh->model_id_, 5 );
            typename pcl::PointCloud<PointT>::Ptr model_aligned (new pcl::PointCloud<PointT>);
            pcl::transformPointCloud (*model_cloud, *model_aligned, trans);
            *pRecognizedModels += *model_aligned;
            sensor_msgs::PointCloud2 rec_model;
            pcl::toROSMsg(*model_aligned, rec_model);
            rec_model.header = req.cloud.header;
            response.models_cloud.push_back(rec_model);


            //        pcl::PointCloud<pcl::Normal>::ConstPtr normal_cloud = model->getNormalsAssembled ( resolution_ );

            //        pcl::PointCloud<pcl::Normal>::Ptr normal_aligned (new pcl::PointCloud<pcl::Normal>);
            //        transformNormals(*normal_cloud, *normal_aligned, oh->transform_);
            //      VisibilityReasoning<pcl::PointXYZRGB> vr (focal_length, img_width, img_height);
            //      vr.setThresholdTSS (0.01f);
            //      /*float fsv_ratio =*/ vr.computeFSVWithNormals (scene_, model_aligned, normal_aligned);
            //      confidence = 1.f - vr.getFSVUsedPoints() / static_cast<float>(model_aligned->points.size());
            //      response.confidence.push_back(confidence);

            // draw bounding box
            Eigen::Vector4f centroid;
            pcl::compute3DCentroid(*model_aligned, centroid);
            geometry_msgs::Point32 centroid_msg;
            centroid_msg.x = centroid[0];
            centroid_msg.y = centroid[1];
            centroid_msg.z = centroid[2];
            response.centroid.push_back(centroid_msg);

            Eigen::Vector4f min;
            Eigen::Vector4f max;
            pcl::getMinMax3D (*model_aligned, min, max);

            geometry_msgs::Polygon bbox;
            bbox.points.resize(8);
            geometry_msgs::Point32 pt;
            pt.x = min[0]; pt.y = min[1]; pt.z = min[2]; bbox.points[0] = pt;
            pt.x = min[0]; pt.y = min[1]; pt.z = max[2]; bbox.points[1] = pt;
            pt.x = min[0]; pt.y = max[1]; pt.z = min[2]; bbox.points[2] = pt;
            pt.x = min[0]; pt.y = max[1]; pt.z = max[2]; bbox.points[3] = pt;
            pt.x = max[0]; pt.y = min[1]; pt.z = min[2]; bbox.points[4] = pt;
            pt.x = max[0]; pt.y = min[1]; pt.z = max[2]; bbox.points[5] = pt;
            pt.x = max[0]; pt.y = max[1]; pt.z = min[2]; bbox.points[6] = pt;
            pt.x = max[0]; pt.y = max[1]; pt.z = max[2]; bbox.points[7] = pt;
            response.bbox.push_back(bbox);

            int min_u, min_v, max_u, max_v;
            min_u = annotated_img.cols;
            min_v = annotated_img.rows;
            max_u = max_v = 0;

            for(size_t m_pt_id=0; m_pt_id < model_aligned->points.size(); m_pt_id++)
            {
                float x = model_aligned->points[m_pt_id].x;
                float y = model_aligned->points[m_pt_id].y;
                float z = model_aligned->points[m_pt_id].z;
                int u = static_cast<int> ( camera_->getFocalLengthX() * x / z + camera_->getCx());
                int v = static_cast<int> ( camera_->getFocalLengthY()  * y / z +  camera_->getCy());

                if (u >= annotated_img.cols || v >= annotated_img.rows || u < 0 || v < 0)
                    continue;

                if(u < min_u)
                    min_u = u;

                if(v < min_v)
                    min_v = v;

                if(u > max_u)
                    max_u = u;

                if(v > max_v)
                    max_v = v;
            }

            cv::rectangle(annotated_img, cv::Point(min_u, min_v), cv::Point(max_u, max_v), cv::Scalar( 0, 255, 255 ), 2);

            // draw coordinate system
            float size=0.1;
            float thickness = 4;
            const Eigen::Matrix3f &R = trans.topLeftCorner<3,3>();
            const Eigen::Vector3f &t = trans.block<3, 1>(0,3);

            Eigen::Vector3f pt0  = R * Eigen::Vector3f(0,0,0) + t;
            Eigen::Vector3f pt_x = R * Eigen::Vector3f(size,0,0) + t;
            Eigen::Vector3f pt_y = R * Eigen::Vector3f(0,size,0) + t;
            Eigen::Vector3f pt_z = R * Eigen::Vector3f(0,0,size) +t ;

            cv::Point2f im_pt0, im_pt_x, im_pt_y, im_pt_z;

//            if (!dist_coeffs.empty())
//            {
//                v4r::projectPointToImage(&pt0 [0], &intrinsic(0), &dist_coeffs(0), &im_pt0.x );
//                v4r::projectPointToImage(&pt_x[0], &intrinsic(0), &dist_coeffs(0), &im_pt_x.x);
//                v4r::projectPointToImage(&pt_y[0], &intrinsic(0), &dist_coeffs(0), &im_pt_y.x);
//                v4r::projectPointToImage(&pt_z[0], &intrinsic(0), &dist_coeffs(0), &im_pt_z.x);
//            }
//            else
            {
                v4r::projectPointToImage(&pt0 [0], &intrinsic[0], &im_pt0.x );
                v4r::projectPointToImage(&pt_x[0], &intrinsic[0], &im_pt_x.x);
                v4r::projectPointToImage(&pt_y[0], &intrinsic[0], &im_pt_y.x);
                v4r::projectPointToImage(&pt_z[0], &intrinsic[0], &im_pt_z.x);
            }

            cv::line(annotated_img, im_pt0, im_pt_x, CV_RGB(255,0,0), thickness);
            cv::line(annotated_img, im_pt0, im_pt_y, CV_RGB(0,255,0), thickness);
            cv::line(annotated_img, im_pt0, im_pt_z, CV_RGB(0,0,255), thickness);

            cv::Point text_start;
            text_start.x = min_u;
            text_start.y = std::max(0, min_v - 10);
            cv::putText(annotated_img, oh->model_id_, text_start, cv::FONT_HERSHEY_COMPLEX_SMALL, 0.8, cv::Scalar(255,0,255), 1, CV_AA);
        }
    }

    sensor_msgs::PointCloud2 recognizedModelsRos;
    pcl::toROSMsg (*pRecognizedModels, recognizedModelsRos);
    recognizedModelsRos.header = req.cloud.header;
    vis_pc_pub_.publish(recognizedModelsRos);

    sensor_msgs::ImagePtr msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", annotated_img).toImageMsg();
    msg->header = req.cloud.header;
    image_pub_.publish(msg);

    return true;
}

template<typename PointT>
bool
RecognizerROS<PointT>::setCamera (v4r_object_recognition_msgs::set_camera::Request & req,
                                  v4r_object_recognition_msgs::set_camera::Response & response)
{
    v4r::Camera::Ptr cam;
    if( req.cam.K[0] < std::numeric_limits<float>::epsilon() )
    {
        ROS_WARN("Given camera calibration matrix has focal length 0. Using default settings");
        cam.reset(new v4r::Camera());
    }
    else
    {
        cam.reset(new v4r::Camera (req.cam.K[0], req.cam.K[4], req.cam.width, req.cam.height, req.cam.K[2], req.cam.K[5] ));
    }

    camera_ = cam;
    mrec_->setCamera( cam );
    (void)response;
}

template<typename PointT>
bool
RecognizerROS<PointT>::recognizeROS(v4r_object_recognition_msgs::recognize::Request &req,
                                    v4r_object_recognition_msgs::recognize::Response &response)
{
    scene_.reset(new pcl::PointCloud<PointT>());
    pcl::fromROSMsg (req.cloud, *scene_);
    scene_->sensor_orientation_ = Eigen::Quaternionf( req.transform.rotation.w, req.transform.rotation.x, req.transform.rotation.y, req.transform.rotation.z );
    scene_->sensor_origin_ = Eigen::Vector4f( req.transform.translation.x, req.transform.translation.y, req.transform.translation.z, 0.f ); ///NOTE: In PCL the last component always seems to be set to 0. Not sure what it does though. Behaves differently if set to 1.

    object_hypotheses_ = mrec_->recognize( scene_ );

    for(size_t ohg_id=0; ohg_id<object_hypotheses_.size(); ohg_id++)
    {
        for(const v4r::ObjectHypothesis::Ptr &oh : object_hypotheses_[ohg_id].ohs_)
        {
            const std::string &model_id = oh->model_id_;
            const Eigen::Matrix4f &tf = oh->pose_refinement_ * oh->transform_;

            if( oh->is_verified_ )
              ROS_INFO_STREAM("********************" << model_id << std::endl << tf << std::endl);
        }
    }

    return respondSrvCall(req, response);
}

template<typename PointT>
bool
RecognizerROS<PointT>::initialize (int argc, char ** argv)
{
    n_.reset( new ros::NodeHandle ( "~" ) );

    // This should get all console arguments except the ROS ones
    std::vector<std::string> arguments(argv + 1, argv + argc);

    std::string models_dir = "", cfg_dir = "";
    if( !n_->getParam ( "models_dir", models_dir ) )
    {
        ROS_ERROR("Models directory is not set. Must be set with ROS parameter \"models_dir\"!");
        return false;
    }
    else
    {
        arguments.push_back("-m");
        arguments.push_back(models_dir);
    }

    if( !n_->getParam ( "cfg_dir", cfg_dir ) )
    {
        ROS_ERROR("The directory containing the XML config folders for object recognition is not set. Must be set with ROS parameter \"cfg_dir\"!");
        return false;
    }

    if(arguments.empty())
    {
        std::string additional_arguments;
        if( n_->getParam ( "arg", additional_arguments ) )
        {
            std::vector<std::string> strs;
            boost::split( strs, additional_arguments, boost::is_any_of("\t ") );
            arguments.insert( arguments.end(), strs.begin(), strs.end() );
        }
    }

    std::cout << "Initializing recognizer with: " << std::endl;
    for( auto arg : arguments )
        std::cout << arg << " ";
    std::cout << std::endl;

    mrec_.reset(new v4r::apps::ObjectRecognizer<PointT>);
    mrec_->initialize(arguments, cfg_dir);
    camera_ = mrec_->getCamera();

    ROS_INFO("Ready to get service calls.");

    vis_pc_pub_ = n_->advertise<sensor_msgs::PointCloud2>( "recogniced_object_instances", 1 );
    recognize_  = n_->advertiseService ("recognize", &RecognizerROS::recognizeROS, this);
    recognizer_set_camera_  = n_->advertiseService ("set_camera", &RecognizerROS::setCamera, this);

    it_.reset(new image_transport::ImageTransport(*n_));
    image_pub_ = it_->advertise("recogniced_object_instances_img", 1, true);

    ROS_INFO("Ready to get service calls.");
    return true;
}

}

int
main (int argc, char ** argv)
{
    ros::init (argc, argv, "object_recognition");
    v4r::RecognizerROS<pcl::PointXYZRGB> m;
    m.initialize (argc, argv);
    ros::spin ();
    return 0;
}
