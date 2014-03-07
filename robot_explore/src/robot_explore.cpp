#include <ros/ros.h>
#include <actionlib/server/simple_action_server.h>
#include <actionlib/client/simple_action_client.h>
#include <costmap_2d/costmap_2d_ros.h>
#include <costmap_2d/costmap_2d.h>

#include <tf/transform_broadcaster.h>

#include <geometry_msgs/PolygonStamped.h>

#include <robot_explore/ExploreTaskAction.h>
#include <robot_explore/GetNextFrontier.h>
#include <robot_explore/UpdateBoundaryPolygon.h>

#include <move_base_msgs/MoveBaseAction.h>

class RobotExplore
{
protected:

    ros::NodeHandle nh_;
    ros::NodeHandle private_nh_;
    tf::TransformListener tf_listener_;
    actionlib::SimpleActionServer<robot_explore::ExploreTaskAction> as_;
    std::string action_name_;

public:

    RobotExplore(std::string name) :
        tf_listener_(ros::Duration(10.0)),
        private_nh_("~"),
        as_(nh_, name, boost::bind(&RobotExplore::executeCB, this, _1), false),
        action_name_(name)
    {

        as_.start();

        //Autoexecute for debug
        actionlib::SimpleActionClient<robot_explore::ExploreTaskAction> exploreClient(ros::this_node::getName(), true);
        robot_explore::ExploreTaskGoal goal;

        double adj_y = 0.25;//0.25;
        double adj_x = 0;//-0.1;

        goal.room_center.header.frame_id = "buildingmap";
        goal.room_center.point.x = 15 + adj_x;
        goal.room_center.point.y = 13 + adj_y;


        goal.room_boundary.header.frame_id = "buildingmap";
        geometry_msgs::Point32 temp;
        temp.x = 19.3257 + adj_x;
        temp.y = 15.7174 + adj_y;
        goal.room_boundary.polygon.points.push_back(temp);
        temp.x = 19.8346 + adj_x;
        temp.y = 14.6269 + adj_y;
        goal.room_boundary.polygon.points.push_back(temp);
        temp.x = 15.4353 + adj_x;
        temp.y = 12.2612 + adj_y;
        goal.room_boundary.polygon.points.push_back(temp);
        temp.x = 14.2299 + adj_x;
        temp.y = 14.2701 + adj_y;
        goal.room_boundary.polygon.points.push_back(temp);

        exploreClient.sendGoal(goal);
    }

    ~RobotExplore(void)
    {
    }

    void executeCB(const robot_explore::ExploreTaskGoalConstPtr &goal)
    {

        int retry;
        //create exploration costmap
        costmap_2d::Costmap2DROS explore_costmap_ros("explore_costmap", tf_listener_);

        //wait for boundary service to come online
        ros::ServiceClient updateBoundaryPolygon = private_nh_.serviceClient<robot_explore::UpdateBoundaryPolygon>("explore_costmap/explore_boundary/update_boundary_polygon");
        if(!updateBoundaryPolygon.waitForExistence()){
            as_.setAborted();
            return;
        }      

        //set region boundary on costmap
        retry = 5;
        while(ros::ok()){
            robot_explore::UpdateBoundaryPolygon srv;
            srv.request.room_boundary = goal->room_boundary;
            if(updateBoundaryPolygon.call(srv)){
                ROS_INFO("set region boundary");
                break;
            }else{
                ROS_ERROR("failed to set region boundary");
                retry--;
                if(retry == 0 || !ros::ok()){
                    as_.setAborted();
                    return;
                }
                ROS_WARN("retrying...");
                ros::Duration(0.5).sleep();
            }
        }

        //connect to move_base
        actionlib::SimpleActionClient<move_base_msgs::MoveBaseAction> moveClient("move_base",true);
        ROS_ERROR("waiting for move_base");
        if(!moveClient.waitForServer()){
            as_.setAborted();
            return;
        }
        ROS_ERROR("found move_base");

        //move to room center
        retry = 5;
        while(ros::ok()){
            move_base_msgs::MoveBaseGoal moveClientGoal;
            moveClientGoal.target_pose.header = goal->room_center.header;
            moveClientGoal.target_pose.pose.position = goal->room_center.point;
            moveClientGoal.target_pose.pose.orientation = getOrientationTangentToGoal(goal->room_center);
            ROS_INFO("moving robot to center of region");
//            moveClient.sendGoalAndWait(moveClientGoal);
            moveClient.sendGoal(moveClientGoal);
            ros::Duration(0.5).sleep();
            moveClient.cancelAllGoals();
            return;
            actionlib::SimpleClientGoalState moveClientState = moveClient.getState();
            if(moveClientState.state_ == actionlib::SimpleClientGoalState::SUCCEEDED){
                ROS_INFO("moved to center");
                break;
            }else{
                ROS_ERROR("failed to move to center");
                retry--;
                if(retry == 0 || !ros::ok()){
                    as_.setAborted();
                    return;
                }
                ROS_WARN("retrying...");
                ros::Duration(0.5).sleep();
            }
        }

        //wait for frontier calculation service to come online
        ros::ServiceClient getNextFrontier = private_nh_.serviceClient<robot_explore::GetNextFrontier>("explore_costmap/explore_boundary/get_next_frontier");
        if(!getNextFrontier.waitForExistence()){
            as_.setAborted();
            return;
        };

        bool success = false;
        //loop until all frontiers are explored (can't find any more)
        while(ros::ok()){

            robot_explore::GetNextFrontier srv;
            srv.request.robot_position = getRobotPositionInFrame("base_link");

            //should return false if done exploring room
            ROS_INFO("calculating frontiers");

            retry = 5;
            while(ros::ok()){
                if(getNextFrontier.call(srv)){
                    ROS_INFO("Found frontier to explore");
                    break;
                }else{
                    ROS_INFO("Couldn't find a frontier");
                    retry--;
                    if(retry == 0 && success){
                        ROS_INFO("Finished exploring room");
                        as_.setSucceeded();
                        return;
                    }else if(retry == 0 || !ros::ok()){
                        ROS_ERROR("Failed exploration");
                        as_.setAborted();
                        return;
                    }
                    //                    ros::Duration(0.2).sleep();
                }

            }

            ROS_INFO_STREAM("Closest frontier " << srv.response.next_frontier.header.frame_id << " " << srv.response.next_frontier.point.x << " " << srv.response.next_frontier.point.y << " " << srv.response.next_frontier.point.z);
            geometry_msgs::PointStamped robot = getRobotPositionInFrame(srv.response.next_frontier.header.frame_id);
            ROS_INFO_STREAM("Robot is at " << robot.header.frame_id << " " << robot.point.x << " " << robot.point.y << " " << robot.point.z);
            geometry_msgs::Point halfway = getPointPartwayToGoal(srv.response.next_frontier, 0.5);
            ROS_INFO_STREAM("Halfway is at " << halfway.x << " " << halfway.y << " " << halfway.z);
//            geometry_msgs::Quaternion yaw = getOrientationTangentToGoal(srv.response.next_frontier);
//            ROS_INFO_STREAM("Yaw is " << 2*acos(yaw.w)*yaw.z/M_PI*180);

            //move halfway to next frontier
            retry = 5;
            while(ros::ok()){
                move_base_msgs::MoveBaseGoal moveClientGoal;
                moveClientGoal.target_pose.header = srv.response.next_frontier.header;
                moveClientGoal.target_pose.pose.position = getPointPartwayToGoal(srv.response.next_frontier, 0.9);
                moveClientGoal.target_pose.pose.orientation = getOrientationTangentToGoal(srv.response.next_frontier);
                ROS_INFO("moving robot 0.9 to next frontier");
                moveClient.sendGoalAndWait(moveClientGoal);
                actionlib::SimpleClientGoalState moveClientState = moveClient.getState();
                if(moveClientState.state_ == actionlib::SimpleClientGoalState::SUCCEEDED){
                    ROS_INFO("moved  halfway to next frontier");
                    success = true;
                    break;
                }else{
                    ROS_ERROR("failed to move halfway to next frontier");
                    retry--;
                    if(retry == 0 || !ros::ok()){
                        as_.setAborted();
                        return;
                    }
                    ROS_WARN("retrying...");
                    ros::Duration(0.5).sleep();
                }
            }

            //            ROS_INFO("spin");
            //            ros::Duration(0.5).sleep();
        }

    }

    geometry_msgs::PointStamped getRobotPositionInFrame(std::string frame){

        //return current robot position transformed into requested frame
        geometry_msgs::PointStamped robot_position;
        robot_position.header.frame_id = "base_link";
        robot_position.point.x = 0;
        robot_position.point.y = 0;
        robot_position.point.z = 0;
        robot_position.header.stamp = ros::Time::now();

        //no transform needed
        if(frame == "base_link"){
            return robot_position;
        }

        bool getTransform = tf_listener_.waitForTransform(frame, robot_position.header.frame_id,ros::Time::now(),ros::Duration(10));
        if(getTransform == false) {
            ROS_ERROR_STREAM("Couldn't transform from "<<frame<<" to "<< robot_position.header.frame_id);
        };

        //transform to target frame
        geometry_msgs::PointStamped temp = robot_position;
        tf_listener_.transformPoint(frame,temp,robot_position);
        return robot_position;

    }

    geometry_msgs::Quaternion getOrientationTangentToGoal(geometry_msgs::PointStamped goalPoint){

        geometry_msgs::PointStamped robot_position = getRobotPositionInFrame(goalPoint.header.frame_id);

        //find desired yaw of robot, tangent to the path from the current position to the goal
        double delta_x, delta_y;
        delta_x = goalPoint.point.x - robot_position.point.x;
        delta_y = goalPoint.point.y - robot_position.point.y;
        ROS_ERROR_STREAM("From point "<<robot_position.point.x<< " "<<robot_position.point.y);
        ROS_ERROR_STREAM("  To point "<<goalPoint.point.x<< " "<<goalPoint.point.y);
        ROS_ERROR_STREAM("      Diff "<<delta_x<<" "<<delta_y);
        double yaw = atan(delta_x/delta_y);
        if(delta_x < 0){
            M_PI-yaw;
        }
        ROS_ERROR_STREAM("       Yaw " << yaw*180.0/M_PI);
        return tf::createQuaternionMsgFromYaw(yaw);

    }

    geometry_msgs::Point getPointPartwayToGoal(geometry_msgs::PointStamped goalPoint, double fraction){

        geometry_msgs::PointStamped robot_position = getRobotPositionInFrame(goalPoint.header.frame_id);
        geometry_msgs::Point out;

        //calculate position partway between robot and goal point
        out.x = (goalPoint.point.x - robot_position.point.x)*fraction + robot_position.point.x;
        out.y = (goalPoint.point.y - robot_position.point.y)*fraction + robot_position.point.y;
        out.z = (goalPoint.point.z - robot_position.point.z)*fraction + robot_position.point.z;

        return out;
    }

};

int main(int argc, char** argv)
{
    ros::init(argc, argv, "robot_explore");

    RobotExplore robot_explore(ros::this_node::getName());
    ros::MultiThreadedSpinner spinner(4);
    spinner.spin();


    return 0;
}
