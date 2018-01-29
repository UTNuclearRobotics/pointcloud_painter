

#include "pointcloud_painter/pointcloud_painter.h"

PointcloudPainter::PointcloudPainter()
{
	std::string service_name;
	nh_.param<std::string>("/pointcloud_painter/service_name", service_name, "/pointcloud_painter/paint");
	ROS_INFO_STREAM("[PointcloudPainter] Initializing service with name " << service_name << ".");

	ros::ServiceServer painter = nh_.advertiseService(service_name, &PointcloudPainter::paint_pointcloud, this);

	ros::spin();
}

/* Paint_Pointcloud - colors a pointcloud using RGB data from a spherical image
 	Inputs
 	 - Input Cloud (generated by laser scan)
 	 - Input Image (spherical, full 360 view)
 	 - Image Frame - assumes INTO image is Z, Horizontal is X, Vertical is Y

*/
bool PointcloudPainter::paint_pointcloud(pointcloud_painter::pointcloud_painter_srv::Request &req, pointcloud_painter::pointcloud_painter_srv::Response &res)
{
	ROS_INFO_STREAM("[PointcloudPainter] Received call to paint pointcloud!");
	ROS_INFO_STREAM("[PointcloudPainter]   Input cloud size: " << req.input_cloud.height*req.input_cloud.width);
	ROS_INFO_STREAM("[PointcloudPainter]   Front image size: " << req.image_front.height << " by " << req.image_front.width);
	ROS_INFO_STREAM("[PointcloudPainter]   Rear image size: " << req.image_rear.height << " by " << req.image_rear.width);

	// Image Angular Resolution
	float hor_res; 	// horizontal
	float ver_res;  // vertical

	//tf2::Transform transform_to_image;

	// ------ Transform input_cloud to camera_frame ------
	std::string cloud_frame = req.input_cloud.header.frame_id;
	tf::TransformListener listener;
	sensor_msgs::PointCloud2 transformed_cloud;
	if(listener.waitForTransform(cloud_frame, req.image_frame, ros::Time::now(), ros::Duration(0.5)))  
	{
		pcl_ros::transformPointCloud (req.image_frame, req.input_cloud, transformed_cloud, listener);  	// transforms input_pc2 into process_message
	}
	else 
	{  													// if Transform request times out... Continues WITHOUT TRANSFORM
		ROS_WARN_THROTTLE(60, "[PointcloudPainter] listen for transformation from %s to %s timed out. Returning paint_pointcloud service unsuccessfully...", cloud_frame.c_str(), req.image_frame.c_str());
		// un-comment this later...
		//return false;
	}

	// ------ Create PCL Pointclouds ------
	pcl::PointCloud<pcl::PointXYZ> input_pcl = pcl::PointCloud<pcl::PointXYZ>();
	pcl::PointCloud<pcl::PointXYZRGB> output_pcl = pcl::PointCloud<pcl::PointXYZRGB>();
	pcl::fromROSMsg(transformed_cloud, input_pcl); 	// Initialize input cloud 

	// ------ Create PCL Pointclouds for Second Method ------
	//   These only matter for the K Nearest Neighbors approach (not for interpolation)
	// Input Cloud - projected onto a sphere of fixed radius
	float projection_radius = 5; // meters
	pcl::PointCloud<pcl::PointXYZRGB> input_pcl_projected = pcl::PointCloud<pcl::PointXYZRGB>(); 
	// Actually perform projection: 
	for(int i=0; i<input_pcl.points.size(); i++)
	{
		float distance = sqrt( pow(input_pcl.points[i].x,2) + pow(input_pcl.points[i].y,2) + pow(input_pcl.points[i].z,2) );
		input_pcl_projected.points[i].x = input_pcl.points[i].x * (projection_radius / distance);
		input_pcl_projected.points[i].y = input_pcl.points[i].y * (projection_radius / distance);
		input_pcl_projected.points[i].z = input_pcl.points[i].z * (projection_radius / distance);
	}
	// Build cloud from Input Image - XYZRGB cloud fixed on a sphere of the same radius as above
	pcl::PointCloud<pcl::PointXYZRGB> image_pcl = pcl::PointCloud<pcl::PointXYZRGB>(); // NOTE - although 

	cv_bridge::CvImagePtr cv_ptr_front; 
	try
	{
		cv_ptr_front = cv_bridge::toCvCopy(req.image_front, sensor_msgs::image_encodings::BGR8);
	}
	catch(cv_bridge::Exception& e)
	{
		ROS_ERROR_STREAM("[PointcloudPainter] cv_bridge exception: " << e.what());
		return false; 
	}
	cv_bridge::CvImagePtr cv_ptr_rear; 
	try
	{
		cv_ptr_rear = cv_bridge::toCvCopy(req.image_rear, sensor_msgs::image_encodings::BGR8);
	}
	catch(cv_bridge::Exception& e)
	{
		ROS_ERROR_STREAM("[PointcloudPainter] cv_bridge exception: " << e.what());
		return false; 
	}

	
	int image_hgt = req.image_front.height;
	int image_wdt = req.image_front.width;
	for(int i=0; i<image_hgt; i++)
	{
		for(int j=0; j<image_wdt; j++)
		{
			pcl::PointXYZRGB point;
			point.x = i;
			point.y = j;
			point.z = 0;

			// cv_bridge::CVImagePtr->image returns a cv::Mat, which allows pixelwise access
			//   https://answers.ros.org/question/187649/pointer-image-multi-channel-access/
			//   Note - data is saved in BGR format (not RGB)
			point.b = cv_ptr_front->image.at<cv::Vec3b>(i,j)[0];
			point.g = cv_ptr_front->image.at<cv::Vec3b>(i,j)[1];
			point.r = cv_ptr_front->image.at<cv::Vec3b>(i,j)[2];

			/*
			// Do something to extract RGB data at X/Y positions...
			//float R = req.data[j + i*image_wdt];
			float R, G, B;
			float X, Y;
			X = 

			point.x = 2*X/(1 + pow(X,2) + pow(Y,2));
			point.y = 2*Y/(1 + pow(X,2) + pow(Y,2));
			point.z = (-1 + pow(X,2) + pow(Y,2))/(1 + pow(X,2) + pow(Y,2));  */

			image_pcl.points.push_back(point);
		}
	}  

	sensor_msgs::PointCloud2 image;
	pcl::toROSMsg(image_pcl, image);
	image.header.frame_id = "map";
	ros::Publisher pub = nh_.advertise<sensor_msgs::PointCloud2>("image_out", 1, this);
	//while(ros::ok())
	{
		pub.publish(image);
		ros::Duration(5).sleep();
	}

	// ------ Populate Output Cloud ------
	for(int i=0; i<input_pcl.points.size(); i++)
	{
		// ------ Populate XYZ Values ------
		pcl::PointXYZRGB point;  // Output point 
		point.x = input_pcl.points[i].x;
		point.y = input_pcl.points[i].y;
		point.z = input_pcl.points[i].z;

		// ------------------ FIRST METHOD ------------------
		/*
		// Assumes clean indexing for square interpolation - probably not as reasonable to implement
		// Expect it to be tricky to find the four relevant pixels in RGB image for each target position 
		// ------ Find Pixels ------
		// Point Angular Location
		float ver_pos = atan2(point.y, point.z); 	// Vertical Angle to Target Point from Z Axis (rotation about X Axis)
		float hor_pos = atan2(point.x, point.z); 	// Horizontal Angle to Target Point from Z Axis (rotation about -Y Axis)
		// Found Pixels - Colors
		float r_ll, g_ll, b_ll; 	// RGB (left, lower)
		float r_rl, g_rl, b_rl; 	// RGB (right, lower)
		float r_lu, g_lu, b_lu; 	// RGB (left, upper)
		float r_ru, g_ru, b_ru; 	// RGB (right, upper)

		// ------ Find Color ------
		// Interpolation
		//   Left Virtual Pixel   	(Vertical Interpolation)
		float r_l = r_ll + (r_lu - r_ll)*ver_res/ver_pos;
		float g_l = g_ll + (g_lu - g_ll)*ver_res/ver_pos;
		float b_l = b_ll + (b_lu - b_ll)*ver_res/ver_pos;
		//   Right Virtual Pixel   	(Vertical Interpolation)
		float r_r = r_rl + (r_ru - r_ll)*ver_res/ver_pos;
		float g_r = g_rl + (g_ru - g_ll)*ver_res/ver_pos;
		float b_r = b_rl + (b_ru - b_ll)*ver_res/ver_pos;
		//   Final Pixel   			(Horizontal Interpolation)
		point.r = r_l + (r_r - r_l)*hor_res/hor_pos;
		point.g = g_l + (g_r - g_l)*hor_res/hor_pos;
		point.b = b_l + (b_r - b_l)*hor_res/hor_pos;
		*/ 

		// ------------------ SECOND METHOD ------------------
		// K Nearest Neighbor search for color determination 
		// might be easiest to operate on image if I convert it to openCV format
		// BUT probably a lot more useful if I can avoid that because openCV is a pain to compile etc...

		

		output_pcl.points.push_back(point);
	}



	pcl::toROSMsg(output_pcl, res.output_cloud);

	//transform output_cloud to cloud_frame
	if(listener.waitForTransform(req.image_frame, cloud_frame, ros::Time::now(), ros::Duration(0.5)))  
	{
		sensor_msgs::PointCloud2 transformed_cloud;
		pcl_ros::transformPointCloud (cloud_frame, res.output_cloud, transformed_cloud, listener);  	// transforms input_pc2 into process_message
	}
	else {  													// if Transform request times out... Continues WITHOUT TRANSFORM
		ROS_WARN_THROTTLE(60, "[PointcloudPainter] listen for transformation from %s to %s timed out. Returning paint_pointcloud service unsuccessfully...", req.image_frame.c_str(), cloud_frame.c_str());
		return false;
	}

	return true;
}

int main(int argc, char** argv)
{
	ros::init(argc, argv, "pointcloud_processing_server");

	pcl::console::setVerbosityLevel(pcl::console::L_ALWAYS);

	PointcloudPainter painter;

}