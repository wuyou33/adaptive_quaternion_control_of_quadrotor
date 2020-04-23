#include "controller/attitude_controller.h"

namespace controller {
  AdaptiveController::AdaptiveController(ros::NodeHandle *nh)
    : nh_(*nh)
  {
    AdaptiveController::init();

    // TODO change to subscribe to /attitude published by rosflight
    odom_subscriber_ = nh_.subscribe("/multirotor/truth/NED", 1000,
        &AdaptiveController::odomCallback, this);
    attitude_command_subscriber_ = nh_.subscribe("/attitude_command", 1000,
        &AdaptiveController::commandCallback, this);
    command_publisher_ = nh_.advertise<rosflight_msgs::Command>(
        "/command", 1000);


    debug_attitude = nh_.advertise<rosflight_msgs::Command>(
        "/debug_attitude", 1000);
    debug_attitude_c = nh_.advertise<rosflight_msgs::Command>(
        "/debug_attitude_d", 1000);

  }

  void AdaptiveController::init()
  {
    // Controller params
    k_q_ = 15;
    k_w_ = 4.5;

    // Initialize model parameters
    max_thrust_ = 14.961 * 4; // From gazebo sim, 4 rotor
    max_torque_ = 0.2 * 14.961; // TODO change
    J_ << 0.07, 0, 0,
          0, 0.08, 0,
          0, 0, 0.12; // From .urdf file

    // Initialize variables
    q_ = Eigen::Quaterniond::Identity();
    w_ = Eigen::Vector3d::Zero();
    q_e_ = Eigen::Quaterniond::Identity();
    w_bc_ = Eigen::Vector3d::Zero();

    // Set command signal
    q_c_ = Eigen::Quaterniond::Identity();
    w_c_ = Eigen::Vector3d::Zero();
    w_c_dot_ = Eigen::Vector3d::Zero();
  }

  void AdaptiveController::odomCallback(
      const nav_msgs::Odometry::ConstPtr &msg
      )
  { 
    // Attitude
    q_ = Eigen::Quaterniond(msg->pose.pose.orientation.w,

                            msg->pose.pose.orientation.x,
                            msg->pose.pose.orientation.y,
                            msg->pose.pose.orientation.z);

    // Angular velocity
    w_ << msg->twist.twist.angular.x,
          msg->twist.twist.angular.y,
          msg->twist.twist.angular.z; // TODO should this be negative?

    // Controll loop
    calculateErrors();
    computeInput();
    publishCommand();
    publishDebug();
  }

  void AdaptiveController::publishDebug()
  {
    rosflight_msgs::Command attitude;
    attitude.header.stamp = ros::Time::now();
    Eigen::Vector3d att_vec = QuatToEuler(q_);
    attitude.x = att_vec(0);
    attitude.y = att_vec(1);
    attitude.z = att_vec(2);
    debug_attitude.publish(attitude);

    rosflight_msgs::Command attitude_c;
    attitude_c.header.stamp = ros::Time::now();
    Eigen::Vector3d att_vec_c = QuatToEuler(q_c_);
    attitude_c.x = att_vec_c(0);
    attitude_c.y = att_vec_c(1);
    attitude_c.z = att_vec_c(2);
    debug_attitude_c.publish(attitude_c);
  }

  void AdaptiveController::commandCallback(
      const rosflight_msgs::Command::ConstPtr& msg
      )
  {
    input_.F = msg->F;

    //double x = 0.15 * sin(ros::Time::now().toSec() * 2 * M_PI * 0.4);
    //double y = 0.3 * sin(ros::Time::now().toSec() * 2 * M_PI * 0.5);
    double x = 0.0;
    double y = 0.0;
    double z = M_PI / 2;
    q_c_ = EulerToQuat(z, y, x);
    
    //q_c_ = EulerToQuat(0, msg->y, msg->x);
  }

  void AdaptiveController::calculateErrors()
  {
    // Calulate aToQuaternionttitude error
    q_e_ = q_c_.conjugate() * q_;
    // Calculate angular velocity error
    w_bc_ = w_ - q_e_.conjugate()._transformVector(w_c_);

    //std::cout << "error: \n" << q_e_.w() << std::endl << q_e_.vec() << std::endl << std::endl;
    //std::cout << w_bc:_ << std::endl << std::endl;
  }


  void AdaptiveController::computeInput()
  {

    // Baseline controller
    Eigen::Vector3d cancellation_terms = cross_map(w_) * J_ * w_
      + J_ * (w_c_dot_
          + cross_map(w_c_) * w_c_);

    Eigen::Vector3d feedforward_terms = J_ * (- k_q_ * quat_log_v(quat_plus_map(q_e_)) - k_w_ * w_bc_);

    input_.tau = cancellation_terms + feedforward_terms;
    std::cout << "input_tau:\n" << -input_.tau << std::endl << std::endl;
  }

  void AdaptiveController::publishCommand()
  {
    rosflight_msgs::Command command;
    command.header.stamp = ros::Time::now();
    //command.mode = rosflight_msgs::Command::MODE_ROLLRATE_PITCHRATE_YAWRATE_THROTTLE;
    command.mode = rosflight_msgs::Command::MODE_PASS_THROUGH;

    command.F = input_.F;
    command.x = input_.tau(0) / max_torque_;
    command.y = input_.tau(1) / max_torque_;
    command.z = input_.tau(2);

    command_publisher_.publish(command);
  }

  double AdaptiveController::saturate(double v, double min, double max)
  {
    v = v > max ? max : (
        v < min ? min : v 
        );

    return v;
  }

  // Note: also called hat map in litterature
  Eigen::Matrix3d AdaptiveController::cross_map(Eigen::Vector3d v)
  {
    Eigen::Matrix3d v_hat;
    v_hat << 0, -v(2), v(1),
            v(2), 0, -v(0),
          -v(1), v(0), 0;
    return v_hat;
  }

  // Opposite of hat map
  Eigen::Vector3d AdaptiveController::vee_map(Eigen::Matrix3d v_hat)
  {
    Eigen::Vector3d v;
    v << v_hat(2,1), v_hat(0,2), v_hat(0,1);
    k_q_ = 1.0;
    return v;
  }

  // Returns the direct map of the quaternion logarithm to R^3 (instead of R^4)
  Eigen::Vector3d AdaptiveController::quat_log_v(Eigen::Quaterniond q)
  {
    Eigen::AngleAxis<double> aa(q);
    return (aa.angle() / 2) * aa.axis();
  }

  // Returns the short rotation quaternion with angle <= pi
  Eigen::Quaterniond AdaptiveController::quat_plus_map(Eigen::Quaterniond q)
  {
    return q.w() >= 0 ? q : Eigen::Quaterniond(-q.w(), q.x(), q.y(), q.z());
  }


 
  /*
  Eigen::Vector3d AdaptiveController::QuatToEuler(Eigen::Quaterniond q)

  {
    Eigen::Vector3d euler = q.toRotationMatrix().eulerAngles(0, 1, 2);
    for (int i = 0; i < 3; ++i)
    {
      if (euler(i) > M_PI / 2)
        euler(i) -= M_PI;
      else if (euler(i) < - M_PI / 2)
        euler(i) += M_PI;
    }
    return euler;
  }*/

  /*

  Eigen::Quaterniond AdaptiveController::EulerToQuat(double yaw, double pitch, double roll)
  {
    Eigen::Quaterniond q;
    q = Eigen::AngleAxisd(roll, Eigen::Vector3d::UnitX())
        * Eigen::AngleAxisd(pitch, Eigen::Vector3d::UnitY())
        * Eigen::AngleAxisd(yaw, Eigen::Vector3d::UnitZ());
    return q;
  }
  */

  Eigen::Quaterniond AdaptiveController::EulerToQuat(double yaw, double pitch, double roll)
    // yaw (Z), pitch (Y), roll (X)
  {
    // Abbreviations for the various angular functions
    double cy = cos(yaw * 0.5);
    double sy = sin(yaw * 0.5);
    double cp = cos(pitch * 0.5);
    double sp = sin(pitch * 0.5);
    double cr = cos(roll * 0.5);
    double sr = sin(roll * 0.5);

    Eigen::Quaterniond q;
    q.w() = cr * cp * cy + sr * sp * sy;
    q.x() = sr * cp * cy - cr * sp * sy;
    q.y() = cr * sp * cy + sr * cp * sy;
    q.z() = cr * cp * sy - sr * sp * cy;

    return q;
  }

  Eigen::Vector3d AdaptiveController::QuatToEuler(Eigen::Quaterniond q) {
    Eigen::Vector3d euler;

    // roll (x-axis rotation)
    double sinr_cosp = 2 * (q.w() * q.x() + q.y() * q.z());
    double cosr_cosp = 1 - 2 * (q.x() * q.x() + q.y() * q.y());
    euler(0) = std::atan2(sinr_cosp, cosr_cosp);

    // pitch (y-axis rotation)
    double sinp = 2 * (q.w() * q.y() - q.z() * q.x());
    if (std::abs(sinp) >= 1)
        euler(1) = std::copysign(M_PI / 2, sinp); // use 90 degrees if out of range
    else
        euler(1) = std::asin(sinp);

    // yaw (z-axis rotation)
    double siny_cosp = 2 * (q.w() * q.z() + q.x() * q.y());
    double cosy_cosp = 1 - 2 * (q.y() * q.y() + q.z() * q.z());
    euler(2) = std::atan2(siny_cosp, cosy_cosp);

    return euler;
  } 

} // namespace controller


// TODO move out of ROS node
int main(int argc, char **argv)
{
  ros::init(argc, argv, "adaptive_controller");
  ros::NodeHandle nh;
  controller::AdaptiveController c = controller::AdaptiveController(&nh);
  ros::spin();

  return 0;
}