#include <ros/ros.h>
#include <nav_msgs/Odometry.h>
#include <coax_msgs/CoaxState.h>
#include <coax_msgs/CoaxReachNavState.h>
#include <coax_ros_control/SetControlMode.h>
#include <coax_ros_control/SetTrajectoryType.h>
#include <coax_ros_control/SetTargetPose.h>
#include <coax_msgs/CoaxConfigureComm.h>
#include <coax_msgs/CoaxSetTimeout.h>
#include <coax_msgs/CoaxRawControl.h>

#include <armadillo>
#include <com/sbapi.h>
#include <CoaxRosControl.h>


CoaxRosControl::CoaxRosControl(ros::NodeHandle & n)
{
	reach_nav_state = n.serviceClient<coax_msgs::CoaxReachNavState>("reach_nav_state");
	configure_comm = n.serviceClient<coax_msgs::CoaxConfigureComm>("configure_comm");
	set_timeout = n.serviceClient<coax_msgs::CoaxSetTimeout>("set_timeout");
		
	raw_control_pub = n.advertise<coax_msgs::CoaxRawControl>("rawcontrol",1);
		
	coax_odom_sub = n.subscribe("odom", 1, &CoaxRosControl::coaxOdomCallback, this);
	coax_state_sub = n.subscribe("state", 1, &CoaxRosControl::coaxStateCallback, this);
		
	set_control_mode.push_back(n.advertiseService("set_control_mode", &CoaxRosControl::setControlMode, this));
	set_trajectory_type.push_back(n.advertiseService("set_trajectory_type", &CoaxRosControl::setTrajectoryType, this));
	set_target_pose.push_back(n.advertiseService("set_target_pose", &CoaxRosControl::setTargetPose, this));
	
	CONTROL_MODE = CONTROL_LANDED;
	LOW_POWER_DETECTED = false;
	FIRST_START = false;
	FIRST_HOVER = false;
	FIRST_TRAJECTORY = false;
	FIRST_LANDING = false;
	FIRST_GOTOPOS = false;
	SERVICE_LANDING = false;
	SERVICE_TRAJECTORY = false;
	FIRST_RUN = true;
	
	battery_voltage = 12.22;
	coax_state_age = 0;
	coax_nav_mode = 0;
	raw_control_age = 0;
	
	roll_trim = 0;
	pitch_trim = 0;
	motor_up = prev_motor_up = 0;
	motor_lo = prev_motor_lo = 0;
	servo_roll = 0;
	servo_pitch = 0;
	
	Omega_up = prev_Omega_up = 0;
	Omega_lo = prev_Omega_lo = 0;
	z_bar[0] = prev_z_bar[0] = 0;
	z_bar[1] = prev_z_bar[1] = 0;
	z_bar[2] = prev_z_bar[2] = 1;
	
	IDLE_TIME = 3;
	START_HEIGHT = 0.3;
	RISE_VELOCITY = 0.1; // [m/s]
	RISE_TIME = START_HEIGHT/RISE_VELOCITY;
	GOTOPOS_VELOCITY = 0.1;
	SINK_VELOCITY = 0.1; // [m/s] positive!
	SINK_TIME = START_HEIGHT/SINK_VELOCITY;
	
	imu_p = 0;
	imu_q = 0;
	imu_r = 0;	
	
}
CoaxRosControl::~CoaxRosControl()
{

}
	
//===================
// Service Clients
//===================
	
bool CoaxRosControl::reachNavState(int des_state, float timeout)
{
	coax_msgs::CoaxReachNavState srv;
	srv.request.desiredState = des_state;
	srv.request.timeout = timeout;
	reach_nav_state.call(srv);
	
	return 0;
}

bool CoaxRosControl::configureComm(int frequency, int contents)
{
	coax_msgs::CoaxConfigureComm srv;
	srv.request.frequency = frequency;
	srv.request.contents = contents;
	configure_comm.call(srv);
	
	return 0;
}

bool CoaxRosControl::setTimeout(unsigned int control_timeout_ms, unsigned int watchdog_timeout_ms)
{
	coax_msgs::CoaxSetTimeout srv;
	srv.request.control_timeout_ms = control_timeout_ms;
	srv.request.watchdog_timeout_ms = watchdog_timeout_ms;
	set_timeout.call(srv);
	
	return 0;
}
	
//===================
// Callback Functions
//===================

void CoaxRosControl::coaxStateCallback(const coax_msgs::CoaxState::ConstPtr & message)
{
	battery_voltage = 0.8817*message->battery + 1.5299;
	coax_nav_mode = message->mode.navigation;
	
	if ((battery_voltage < 10.80) && !LOW_POWER_DETECTED){
		ROS_INFO("Battery Low!!! (%fV) Landing initialized",battery_voltage);
		LOW_POWER_DETECTED = true;
	}
	
	imu_p = message->gyro[0];
	imu_q = -message->gyro[1];
	imu_r = -message->gyro[2];
	
	coax_state_age = 0;
}

void CoaxRosControl::coaxOdomCallback(const nav_msgs::Odometry::ConstPtr & message)
{

	// vicon jump detection ?
	
	// getting current time
	time_now = ros::Time::now().toSec();
	if (FIRST_RUN){
		time_prev = time_now;
		FIRST_RUN = 0;
	}
	double delta_t = time_now - time_prev;
	
	// Get orientation quaternion
	double qx = message->pose.pose.orientation.x;
	double qy = message->pose.pose.orientation.y;
	double qz = message->pose.pose.orientation.z;
	double qw = message->pose.pose.orientation.w;
	
	// Compute rotation matrix
	arma::mat Rb2w = arma::zeros(3,3);
	Rb2w(0,0) = 1-2*qy*qy-2*qz*qz;
	Rb2w(0,1) = 2*qx*qy-2*qz*qw;
	Rb2w(0,2) = 2*qx*qz+2*qy*qw;
	Rb2w(1,0) = 2*qx*qy+2*qz*qw;
	Rb2w(1,1) = 1-2*qx*qx-2*qz*qz;
	Rb2w(1,2) = 2*qy*qz-2*qx*qw;
	Rb2w(2,0) = 2*qx*qz-2*qy*qw;
	Rb2w(2,1) = 2*qy*qz+2*qx*qw;
	Rb2w(2,2) = 1-2*qx*qx-2*qy*qy;
	
	// Estimate stabilizer bar orientation
	double b_z_bardotz = 1/model_params.Tf_up*acos(prev_z_bar[2])*sqrt(prev_z_bar[0]*prev_z_bar[0] + prev_z_bar[1]*prev_z_bar[1]);
	arma::colvec b_z_bardot(3);
	if (b_z_bardotz <= 0){
		b_z_bardot = arma::zeros(3);
	}else{
		double temp = prev_z_bar[2]*b_z_bardotz/(prev_z_bar[0]*prev_z_bar[0]+prev_z_bar[1]*prev_z_bar[1]);
		b_z_bardot(0) = -prev_z_bar[0]*temp;
		b_z_bardot(1) = -prev_z_bar[1]*temp;
		b_z_bardot(2) = b_z_bardotz;
	}

	double p = message->twist.twist.angular.x;
	double q = message->twist.twist.angular.y;
	double r = imu_r;
	
	z_bar[0] = prev_z_bar[0] + (r*prev_z_bar[1] - q*prev_z_bar[2] + b_z_bardot(0))*delta_t;
	z_bar[1] = prev_z_bar[1] + (-r*prev_z_bar[0] + p*prev_z_bar[2] + b_z_bardot(1))*delta_t;
	z_bar[2] = prev_z_bar[2] + (q*prev_z_bar[0] - p*prev_z_bar[1] + b_z_bardot(2))*delta_t;
	
	double norm_z_bar = sqrt(z_bar[0]*z_bar[0] + z_bar[1]*z_bar[1] + z_bar[2]*z_bar[2]);
	z_bar[0] = z_bar[0]/norm_z_bar;
	z_bar[1] = z_bar[1]/norm_z_bar;
	z_bar[2] = z_bar[2]/norm_z_bar;
	
	prev_z_bar[0] = z_bar[0];
	prev_z_bar[1] = z_bar[1];
	prev_z_bar[2] = z_bar[2];
	
	// Compose coax_state
	arma::colvec coax_state = arma::zeros(17);
	coax_state(0) = message->pose.pose.position.x;
	coax_state(1) = message->pose.pose.position.y;
	coax_state(2) = message->pose.pose.position.z;
	coax_state(3) = message->twist.twist.linear.x;
	coax_state(4) = message->twist.twist.linear.y;
	coax_state(5) = message->twist.twist.linear.z;	
	coax_state(6) = atan2(2*(qw*qx+qy*qz),1-2*(qx*qx+qy*qy));
	coax_state(7) = asin(2*(qw*qy-qz*qx));
	coax_state(8) = atan2(2*(qw*qz+qx*qy),1-2*(qy*qy+qz*qz));
	coax_state(9) = p;
	coax_state(10) = q;
	coax_state(11) = r;
	coax_state(12) = Omega_up;
	coax_state(13) = Omega_lo;
	coax_state(14) = z_bar[0];
	coax_state(15) = z_bar[1];
	coax_state(16) = z_bar[2];
	
	arma::colvec trajectory = arma::zeros(11);
	double control[4] = {0,0,0,0};	
	
	double dt_start;
	double dt_gotopos;
	double dt_land;
	double dt_traj;
	double gotopos_distance;
	double position_error;
	double init_traj_pose[4];
	int i;
	
	switch (CONTROL_MODE) {
			
		case CONTROL_START:
			
			if (FIRST_START){
				start_position[0] = coax_state(0);
				start_position[1] = coax_state(1);
				start_position[2] = coax_state(2);
				start_orientation = atan2(Rb2w(1,0),Rb2w(0,0));
				start_time = time_now;
            
				FIRST_START = false;
			}
			
			dt_start = time_now - start_time;
			if (dt_start < IDLE_TIME/2){
				control[0] = 0.35;
				control[1] = 0;
				control[2] = 0;
				control[3] = 0;
			}else if (dt_start < IDLE_TIME){
				control[0] = 0.35;
				control[1] = 0.35;
				control[2] = 0;
				control[3] = 0;
			}else if (dt_start < (IDLE_TIME + RISE_TIME)){
				// rise
				trajectory(0) = start_position[0];
				trajectory(1) = start_position[1];
				trajectory(2) = start_position[2] + RISE_VELOCITY*(dt_start - IDLE_TIME);
				trajectory(5) = RISE_VELOCITY;
				trajectory(9) = start_orientation;
				
				// compute control commands
				controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
			}else{
				CONTROL_MODE = CONTROL_HOVER;
				// No FIRST_HOVER required !!!
				hover_position[0] = start_position[0];
				hover_position[1] = start_position[1];
				hover_position[2] = start_position[2] + START_HEIGHT;
				hover_orientation = start_orientation;
				
				trajectory(0) = hover_position[0];
				trajectory(1) = hover_position[1];
				trajectory(2) = hover_position[2];
				trajectory(9) = hover_orientation;
				
				// compute control commands
				controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
			}
			
			break;
		
		case CONTROL_HOVER:
			
			if (FIRST_HOVER) {
				hover_position[0] = coax_state(0);
				hover_position[1] = coax_state(1);
				hover_position[2] = coax_state(2);
				hover_orientation = atan2(Rb2w(1,0),Rb2w(0,0));
				FIRST_HOVER = 0;
			}
			
			// compose trajectory
			trajectory(0) = hover_position[0];
			trajectory(1) = hover_position[1];
			trajectory(2) = hover_position[2];
			trajectory(9) = hover_orientation;
			
			// compute control commands
			controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
			
			if (LOW_POWER_DETECTED) {
				CONTROL_MODE = CONTROL_LANDING;
				FIRST_LANDING = true;
			}
			
			break;
		
		case CONTROL_GOTOPOS:
			
			if (FIRST_GOTOPOS){
				initial_gotopos_position[0] = coax_state(0);
				initial_gotopos_position[1] = coax_state(1);
				initial_gotopos_position[2] = coax_state(2);
				initial_gotopos_orientation = atan2(Rb2w(1,0),Rb2w(0,0));
				gotopos_time = time_now;
				gotopos_distance = 0;
				for (i=0; i<3; i++) {
					gotopos_distance += (gotopos_position[i] - initial_gotopos_position[i])*(gotopos_position[i] - initial_gotopos_position[i]);
				}
				gotopos_distance = sqrt(gotopos_distance);
				gotopos_direction[0] = (gotopos_position[0] - initial_gotopos_position[0])/gotopos_distance;
				gotopos_direction[1] = (gotopos_position[1] - initial_gotopos_position[1])/gotopos_distance;
				gotopos_direction[2] = (gotopos_position[2] - initial_gotopos_position[2])/gotopos_distance;
				gotopos_rot_distance = gotopos_orientation - initial_gotopos_orientation;
				while (gotopos_rot_distance > M_PI){
					gotopos_rot_distance -= 2*M_PI;
				}
				while (gotopos_rot_distance < -M_PI){
					gotopos_rot_distance += 2*M_PI;
				}
				
				gotopos_duration = gotopos_distance/GOTOPOS_VELOCITY;
				FIRST_GOTOPOS = false;
			}
			
			dt_gotopos = time_now - gotopos_time;
			if (dt_gotopos < gotopos_duration){
				// compose trajectory
				trajectory(0) = initial_gotopos_position[0] + GOTOPOS_VELOCITY*dt_gotopos*gotopos_direction[0];
				trajectory(1) = initial_gotopos_position[1] + GOTOPOS_VELOCITY*dt_gotopos*gotopos_direction[1];
				trajectory(2) = initial_gotopos_position[2] + GOTOPOS_VELOCITY*dt_gotopos*gotopos_direction[2];
				trajectory(3) = GOTOPOS_VELOCITY*gotopos_direction[0];
				trajectory(4) = GOTOPOS_VELOCITY*gotopos_direction[1];
				trajectory(5) = GOTOPOS_VELOCITY*gotopos_direction[2];
				trajectory(9) = dt_gotopos/gotopos_duration*gotopos_rot_distance+initial_gotopos_orientation;
				trajectory(10) = gotopos_rot_distance/gotopos_duration;

				// compute control commands
				controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
			}else{
				if (SERVICE_LANDING){
					CONTROL_MODE = CONTROL_LANDING;
					SERVICE_LANDING = false;
					
					// compose trajectory
					trajectory(0) = coax_state(0);
					trajectory(1) = coax_state(1);
					trajectory(2) = coax_state(2);
					trajectory(9) = gotopos_orientation;
					
					// compute control commands
					controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
				}else if (SERVICE_TRAJECTORY){
					CONTROL_MODE = CONTROL_TRAJECTORY;
					SERVICE_TRAJECTORY = false;
					
					// compose trajectory
					trajectory(0) = coax_state(0);
					trajectory(1) = coax_state(1);
					trajectory(2) = coax_state(2);
					trajectory(9) = gotopos_orientation;
					
					// compute control commands
					controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
				}else{
					position_error = 0;
					for (i=0; i<3; i++) {
						position_error += (coax_state(i)-gotopos_position[i])*(coax_state(i)-gotopos_position[i]);
					}			
					position_error = sqrt(position_error);
					
					if (position_error > 0.1){
						CONTROL_MODE = CONTROL_GOTOPOS;
						FIRST_GOTOPOS = true;
					}else{
						CONTROL_MODE = CONTROL_HOVER; // in the end if manually entered goto position
						hover_position[0] = target_pose[0];
						hover_position[1] = target_pose[1];
						hover_position[2] = target_pose[2];
						hover_orientation = target_pose[3];
					}
					
					// compose trajectory
					trajectory(0) = coax_state(0);
					trajectory(1) = coax_state(1);
					trajectory(2) = coax_state(2);
					trajectory(9) = atan2(Rb2w(1,0),Rb2w(0,0));
					
					// compute control commands
					controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
				}
			}
			
			if (LOW_POWER_DETECTED && !SERVICE_LANDING){
				CONTROL_MODE = CONTROL_HOVER;
				FIRST_HOVER = true;
			}
			
			break;
			
		case CONTROL_TRAJECTORY:
			
			if (FIRST_TRAJECTORY){
				trajectory = trajectoryGeneration(0,TRAJECTORY_TYPE,init_traj_pose);
				
				position_error = 0;
				for (i=0; i<3; i++) {
					position_error += (coax_state(i)-init_traj_pose[i])*(coax_state(i)-init_traj_pose[i]);
				}			
				position_error = sqrt(position_error);
				
				if (position_error > 0.1){
					CONTROL_MODE = CONTROL_GOTOPOS;
					FIRST_GOTOPOS = true;
					SERVICE_TRAJECTORY = true;
					gotopos_position[0] = init_traj_pose[0];
					gotopos_position[1] = init_traj_pose[1];
					gotopos_position[2] = init_traj_pose[2];
					gotopos_orientation = init_traj_pose[3];
				}else{
					FIRST_TRAJECTORY = 0;
					trajectory_time = time_now;
				}
				trajectory(0) = coax_state(0);
				trajectory(1) = coax_state(1);
				trajectory(2) = coax_state(2);
				trajectory(9) = atan2(Rb2w(1,0),Rb2w(0,0));
				
				// compute control commands
				controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
			}else{
				dt_traj = time_now - trajectory_time;
				trajectory = trajectoryGeneration(dt_traj,TRAJECTORY_TYPE,init_traj_pose);
				
				// compute control commands
				controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
			}
			
			if (LOW_POWER_DETECTED){
				CONTROL_MODE = CONTROL_HOVER;
				FIRST_HOVER = true;
			}
			
			break;
		
		case CONTROL_LANDING:
			
			position_error = (coax_state(0)-start_position[0])*(coax_state(0)-start_position[0]);
			position_error += (coax_state(1)-start_position[1])*(coax_state(1)-start_position[1]);
			position_error += (coax_state(2)-start_position[2]-START_HEIGHT)*(coax_state(2)-start_position[2]-START_HEIGHT);			
			position_error = sqrt(position_error);
			if (FIRST_LANDING){
				if (position_error > 0.1){
					CONTROL_MODE = CONTROL_GOTOPOS;
					FIRST_GOTOPOS = true;
					SERVICE_LANDING = true;
					gotopos_position[0] = start_position[0];
					gotopos_position[1] = start_position[1];
					gotopos_position[2] = start_position[2] + START_HEIGHT;
					gotopos_orientation = start_orientation;
				}else{
					FIRST_LANDING = 0;
					landing_time = time_now;
				}
				
				// compose trajectory
				trajectory(0) = coax_state(0);
				trajectory(1) = coax_state(1);
				trajectory(2) = coax_state(2);
				trajectory(9) = start_orientation;
				
				// compute control commands
				controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
			}else{
				dt_land = time_now - landing_time;
				if (dt_land < SINK_TIME){					
					// compose trajectory
					trajectory(0) = start_position[0];
					trajectory(1) = start_position[1];
					trajectory(2) = start_position[2] + START_HEIGHT - SINK_VELOCITY*dt_land;
					trajectory(5) = -SINK_VELOCITY;
					trajectory(9) = start_orientation;
					
					// compute control commands
					controlFunction(control, coax_state, Rb2w, trajectory, model_params, control_params);
				}else if (dt_land < SINK_TIME + IDLE_TIME){
					control[0] = 0.35;
					control[1] = 0.35;
					control[2] = 0;
					control[3] = 0;
				}else{
					CONTROL_MODE = CONTROL_LANDED; // in the end of maneuver
					// flush integrators
					motor_up = 0;
					motor_lo = 0;
					servo_roll = 0;
					servo_pitch = 0;
					reachNavState(SB_NAV_STOP, 0.5);
				}
			}
			
			break;
			
		case CONTROL_LANDED:
			
			// maybe if flags need to be reset
			break;

		default:
			break;
	}
	
	// Voltage compensation
	double volt_compUp = (12.22 - battery_voltage)*0.0279;
	double volt_compLo = (12.22 - battery_voltage)*0.0287;
	if (control[0] > 0.05 || control[1] > 0.05){
		control[0] = control[0] + volt_compUp;
		control[1] = control[1] + volt_compLo;
	}
	
	// set control commands
	setControls(control);
										
	raw_control_age = 0;
	time_prev = time_now;
}
										
//===================
// Control functions
//===================

void CoaxRosControl::controlFunction(double* control, arma::colvec coax_state, arma::mat Rb2w, 
									 arma::colvec trajectory, model_params_t model_params, control_params_t control_params)
{
	
	double z = coax_state(2);
	double zdot = coax_state(5);
	double p = coax_state(9);
	double q = coax_state(10);
	double r = coax_state(11);
	
	// rotor speeds
	double Omega_up = coax_state(12);
	double Omega_lo = coax_state(13);
	
	// stabilizer bar direction
	double z_barx = coax_state(14);
	double z_bary = coax_state(15);
	double z_barz = coax_state(16);
	
	// Parameters
	double g = 9.81;
	double m = model_params.mass;
	//double Ixx = model_params.Ixx;
	//double Iyy = model_params.Iyy;
	//double Izz = model_params.Izz;
	//double d_up = model_params.d_up;
	//double d_lo = model_params.d_lo;
	//double k_springup = model_params.k_springup;
	//double k_springlo = model_params.k_springlo;
	double l_up = model_params.l_up;
	double l_lo = model_params.l_lo;
	double k_Tup = model_params.k_Tup;
	double k_Tlo = model_params.k_Tlo;
	double k_Mup = model_params.k_Mup;
	double k_Mlo = model_params.k_Mlo;
	//double Tf_motup = model_params.Tf_motup;
	//double Tf_motlo = model_params.Tf_motlo;
	//double Tf_up = model_params.Tf_up;
	double rs_mup = model_params.rs_mup;
	double rs_bup = model_params.rs_bup;
	double rs_mlo = model_params.rs_mlo;
	double rs_blo = model_params.rs_blo;
	double zeta_mup = model_params.zeta_mup;
	double zeta_bup = model_params.zeta_bup;
	double zeta_mlo = model_params.zeta_mlo;
	double zeta_blo = model_params.zeta_blo;
	double max_SPangle = model_params.max_SPangle;
	
	// Control Parameters
	double Kp_Fx = control_params.Kp_Fx;
	double Kp_Fy = control_params.Kp_Fy;
	double Kp_Fz = control_params.Kp_Fz;
	double Kd_Fx = control_params.Kd_Fx;
	double Kd_Fy = control_params.Kd_Fy;
	double Kd_Fz = control_params.Kd_Fz;
	double Kp_Mz = control_params.Kp_Mz;
	double Kd_Mz = control_params.Kd_Mz;
	double Kpq_roll = control_params.Kpq_roll;
	double Kpq_pitch = control_params.Kpq_pitch;
	
	// Desired Forces
	arma::colvec kpxy(2);
	arma::colvec kdxy(2);
	arma::colvec kpq(2);
	kpxy(0) = Kp_Fx;
	kpxy(1) = Kp_Fy;
	kdxy(0) = Kd_Fx;
	kdxy(1) = Kd_Fy;
	kpq(0) = Kpq_pitch;
	kpq(1) = Kpq_roll;
	
	arma::colvec pos_error(2);
	arma::colvec vel_error(2);
	arma::colvec pq_error(2);
	pos_error = coax_state.subvec(0, 1) - trajectory.subvec(0, 1);
	vel_error = coax_state.subvec(3, 4) - trajectory.subvec(3, 4);
	pq_error(0) = q;
	pq_error(1) = p;
	
	arma::colvec Fxy_des(2);
	Fxy_des = -arma::diagmat(kpxy)*pos_error - arma::diagmat(kdxy)*vel_error - Rb2w.submat(0,0,1,1)*arma::diagmat(kpq)*pq_error + m*trajectory.subvec(6, 7);
	
	// Upper thrust vector direction
	double z_Tupz = cos(l_up*acos(z_barz));
	arma::colvec z_Tup_p(3);
	double temp;
	if (z_Tupz < 1){
		temp = sqrt((1-z_Tupz*z_Tupz)/(z_barx*z_barx + z_bary*z_bary));
		z_Tup_p(0) = z_barx*temp;
		z_Tup_p(1) = z_bary*temp;
		z_Tup_p(2) = z_Tupz;
	}else {
		z_Tup_p(0) = 0;
		z_Tup_p(1) = 0;
		z_Tup_p(2) = 1;
	}
	double zeta = zeta_mup*Omega_up + zeta_bup;
	arma::colvec z_Tup(3);
	z_Tup(0) = cos(zeta)*z_Tup_p(0) - sin(zeta)*z_Tup_p(1);
	z_Tup(1) = sin(zeta)*z_Tup_p(0) + cos(zeta)*z_Tup_p(1);
	z_Tup(2) = z_Tup_p(2);

	// Lower thrust vector direction
	arma::colvec z_Tlo(3);
	if (Omega_lo < 10) {
		z_Tlo(0) = 0;
		z_Tlo(1) = 0;
		z_Tlo(2) = 1;
	} else {
		z_Tlo(0) = 1/(k_Tlo*Omega_lo*Omega_lo)*(Rb2w(0,0)*Fxy_des(0) + Rb2w(1,0)*Fxy_des(1));
		z_Tlo(1) = 1/(k_Tlo*Omega_lo*Omega_lo)*(Rb2w(0,1)*Fxy_des(0) + Rb2w(1,1)*Fxy_des(1));
		z_Tlo(2) = sqrt(1-z_Tlo(0)*z_Tlo(0)-z_Tlo(1)*z_Tlo(1));
	}
	
	// correct for phase lag of servo inputs
	zeta = zeta_mlo*Omega_lo + zeta_blo;
	arma::colvec z_Tlo_p(3);
	z_Tlo_p(0) = cos(zeta)*z_Tlo(0) - sin(zeta)*z_Tlo(1);
	z_Tlo_p(1) = sin(zeta)*z_Tlo(0) + cos(zeta)*z_Tlo(1);
	z_Tlo_p(2) = z_Tlo(2);
	
	arma::colvec z_SP(3);
	double z_SPz = cos(1/l_lo*acos(z_Tlo_p(2)));
	if (z_SPz < 1){
		temp = sqrt((1-z_SPz*z_SPz)/(z_Tlo_p(0)*z_Tlo_p(0) + z_Tlo_p(1)*z_Tlo_p(1)));
		z_SP(0) = z_Tlo_p(0)*temp;
		z_SP(1) = z_Tlo_p(1)*temp;
		z_SP(2) = z_SPz;
	}else{
		z_SP(0) = 0;
		z_SP(1) = 0;
		z_SP(2) = 1;
	}
	double b_lo_des = asin(z_SP(0));
	double a_lo_des = asin(-z_SP(1)/cos(b_lo_des));
	
	control[2] = a_lo_des/max_SPangle;
	control[3] = b_lo_des/max_SPangle;
	
	// New heave-yaw control
	double Fz_des = -Kp_Fz*(z-trajectory(2)) - Kd_Fz*(zdot-trajectory(5)) + m*trajectory(8);
	double ori_error = atan2(Rb2w(1,0),Rb2w(0,0)) - trajectory(9);
	while (ori_error > M_PI){
		ori_error = ori_error - 2*M_PI;
	}
	while (ori_error < -M_PI){
		ori_error = ori_error + 2*M_PI;
	}
	double Mz_des = -Kp_Mz*ori_error - Kd_Mz*(r-Rb2w(2,2)*trajectory(10));
	
	double A = k_Tup/k_Mup*Mz_des*(arma::as_scalar(Rb2w.row(2)*z_Tup));
	double B = k_Tup/k_Mup*k_Mlo*(arma::as_scalar(Rb2w.row(2)*z_Tup)) + k_Tlo*(arma::as_scalar(Rb2w.row(2)*z_Tlo));
	
	double Omega_lo_des = sqrt((m*g + A + Fz_des)/B);
	double Omega_up_des = sqrt((k_Mlo*Omega_lo_des*Omega_lo_des - Mz_des)/k_Mup);
	control[0] = (Omega_up_des - rs_bup)/rs_mup;
	control[1] = (Omega_lo_des - rs_blo)/rs_mlo;
	
}

arma::colvec CoaxRosControl::trajectoryGeneration(double time, int TYPE, double* init_traj_pose)
{
	arma::colvec trajectory = arma::zeros(11);
	double radius;
	double omega;
	double omega_vert;
	double amplitude;
	double vert_amp;
	double length;
	double vel;
	
	switch (TYPE) {
			//case TRAJECTORY_SPIRAL:
			
			//break;
			
		case TRAJECTORY_ROTINPLACE:
			
			omega      = 2*M_PI/2;
			
			init_traj_pose[0] = 0;
			init_traj_pose[1] = 0;
			init_traj_pose[2] = 1;
			init_traj_pose[3] = 0;
			
			trajectory(0) = init_traj_pose[0];
			trajectory(1) = init_traj_pose[1];
			trajectory(2) = init_traj_pose[2];
			trajectory(9) = omega*time + init_traj_pose[3];
			trajectory(10) = omega;
			
			break;
			
		case TRAJECTORY_VERTOSCIL:
			
			amplitude  = 0.5;
			omega      = 2*M_PI/5;
			
			init_traj_pose[0] = 0;
			init_traj_pose[1] = 0;
			init_traj_pose[2] = 1;
			init_traj_pose[3] = 0;
			
			trajectory(0) = init_traj_pose[0];
			trajectory(1) = init_traj_pose[1];
			trajectory(2) = init_traj_pose[2] + amplitude*sin(omega*time);
			trajectory(5) = amplitude*omega*cos(omega*time);
			trajectory(8) = -amplitude*omega*omega*sin(omega*time);
			trajectory(9) = init_traj_pose[3];
			
			break;
			
		case TRAJECTORY_LYINGCIRCLE:
			
			radius     = 0.5;
			omega      = 2*M_PI/10;
			omega_vert = 2*omega;
			vert_amp   = 0.2;
			
			init_traj_pose[0] = 0.5;
			init_traj_pose[1] = 0;
			init_traj_pose[2] = 1;
			init_traj_pose[3] = -M_PI/2;
			
			trajectory(0) = radius*cos(omega*time) - radius + init_traj_pose[0];
			trajectory(1) = radius*sin(omega*time) + init_traj_pose[1];
			trajectory(2) = vert_amp*sin(omega_vert*time) + init_traj_pose[2];
			trajectory(3) = -radius*omega*sin(omega*time);
			trajectory(4) = radius*omega*cos(omega*time);
			trajectory(5) = omega_vert*vert_amp*cos(omega_vert*time);
			trajectory(6) = -radius*omega*omega*cos(omega*time);
			trajectory(7) = -radius*omega*omega*sin(omega*time);
			trajectory(8) = -omega_vert*omega_vert*vert_amp*sin(omega_vert*time);
			trajectory(9) = init_traj_pose[3]; //+ omega*time;
			trajectory(10) = 0; //omega;
			break;
			
		case TRAJECTORY_STANDINGCIRCLE:
			
			radius = 1;
			omega = 2*M_PI/10;
			
			init_traj_pose[0] = 0;
			init_traj_pose[1] = 0;
			init_traj_pose[2] = 1;
			init_traj_pose[3] = 0;
			
			trajectory(0) = radius*sin(omega*time) + init_traj_pose[0];
			trajectory(1) = init_traj_pose[1];
			trajectory(2) = init_traj_pose[2] + radius - radius*cos(omega*time);
			trajectory(3) = radius*omega*cos(omega*time);
			trajectory(5) = -radius*omega*sin(omega*time);
			trajectory(6) = -radius*omega*omega*sin(omega*time);
			trajectory(8) = -radius*omega*omega*cos(omega*time);
			trajectory(9) = init_traj_pose[3];
			
			break;
			
		case TRAJECTORY_YAWOSCIL:
			
			amplitude  = M_PI/2;
			omega      = 2*M_PI/4;
			
			init_traj_pose[0] = 0;
			init_traj_pose[1] = 0;
			init_traj_pose[2] = 1;
			init_traj_pose[3] = 0;
			
			trajectory(0) = init_traj_pose[0];
			trajectory(1) = init_traj_pose[1];
			trajectory(2) = init_traj_pose[2];
			trajectory(9) = init_traj_pose[3] + amplitude*sin(omega*time);
			trajectory(10) = amplitude*omega*cos(omega*time);
			
			break;
			
		case TRAJECTORY_HORZLINE:
			
			length = 0.5;
			vel = 0.15;
			
			init_traj_pose[0] = 0.5;
			init_traj_pose[1] = 0;
			init_traj_pose[2] = 1;
			init_traj_pose[3] = M_PI;
			
			if (time < length/vel){
				trajectory(0) = init_traj_pose[0] - time*vel;
				trajectory(1) = init_traj_pose[1];
				trajectory(2) = init_traj_pose[2];
				trajectory(3) = -vel;
				trajectory(9) = init_traj_pose[3];
			}else{
				trajectory(0) = init_traj_pose[0] - length;
				trajectory(1) = init_traj_pose[1];
				trajectory(2) = init_traj_pose[2];
				trajectory(9) = init_traj_pose[3];
			}
			
			break;
			
		default:
			init_traj_pose[0] = 0;
			init_traj_pose[1] = 0;
			init_traj_pose[2] = 1;
			init_traj_pose[3] = 0;
			
			trajectory(0) = init_traj_pose[0];
			trajectory(1) = init_traj_pose[1];
			trajectory(2) = init_traj_pose[2];
			trajectory(9) = init_traj_pose[3];
			break;
	}
	
	return trajectory;
}

void CoaxRosControl::setControls(double* control)
{
	if (control[0] > 1) {
		motor_up = 1;
	} else if (control[0] < 0) {
		motor_up = 0;
	} else {
		motor_up = control[0];
	}
	
	if (control[1] > 1) {
		motor_lo = 1;
	} else if (control[1] < 0) {
		motor_lo = 0;
	} else {
		motor_lo = control[1];
	}
	
	if (control[2] > 1) {
		servo_roll = 1;
	} else if (control[2] < -1) {
		servo_roll = -1;
	} else {
		servo_roll = control[2];
	}
	
	if (control[3] > 1) {
		servo_pitch = 1;
	} else if (control[3] < -1) {
		servo_pitch = -1;
	} else {
		servo_pitch = control[3];
	}
}

//===================
// Publisher
//===================

void CoaxRosControl::rawControlPublisher(unsigned int rate)
{
	ros::Rate loop_rate(rate);
	
	coax_msgs::CoaxRawControl raw_control;
	
	while(ros::ok())
	{
		if (raw_control_age < 20) {
			raw_control.motor1 = motor_up;
			raw_control.motor2 = motor_lo;
			raw_control.servo1 = servo_roll + roll_trim;
			raw_control.servo2 = servo_pitch + pitch_trim;
		} else { // if we do not get new control_values for too long -> send zero commands
			raw_control.motor1 = 0;
			raw_control.motor2 = 0;
			raw_control.servo1 = 0;
			raw_control.servo2 = 0;
		}
		
		if ((coax_state_age > 0.5*rate) && (coax_state_age <= 0.5*rate + 1)) {
			ROS_INFO("Lost Zigbee connection for too long!!!");
		}
		
		raw_control_pub.publish(raw_control);
		raw_control_age += 1;
		coax_state_age += 1;
		
		// Estimate rotor speeds (do that here to have the same frequency as we apply inputs)
		double prev_Omega_up_des = model_params.rs_mup*prev_motor_up + model_params.rs_bup;
		double prev_Omega_lo_des = model_params.rs_mlo*prev_motor_lo + model_params.rs_blo;
		//printf("%f   %f \n", prev_Omega_up_des,prev_Omega_lo_des);
		//printf("%f   %f \n", prev_Omega_up,prev_Omega_lo);
		
		Omega_up = prev_Omega_up + 1/model_params.Tf_motup*(prev_Omega_up_des - prev_Omega_up)/rate;
		Omega_lo = prev_Omega_lo + 1/model_params.Tf_motlo*(prev_Omega_lo_des - prev_Omega_lo)/rate;
		prev_Omega_up = Omega_up;
		prev_Omega_lo = Omega_lo;
		prev_motor_up = raw_control.motor1;
		prev_motor_lo = raw_control.motor2;
		//printf("%f   %f \n", model_params.Tf_motup,model_params.Tf_motlo);
		//printf("%f   %f \n\n", Omega_up,Omega_lo);
		
		ros::spinOnce();
		loop_rate.sleep();
	}
}

//===================
// Services
//===================

bool CoaxRosControl::setControlMode(coax_ros_control::SetControlMode::Request &req, coax_ros_control::SetControlMode::Response &out)
{
	out.result = 0;
	
	switch (req.mode)
	{
		case 1:
			if (CONTROL_MODE == CONTROL_LANDED){
				if (battery_voltage > 11) {
					if (coax_nav_mode != SB_NAV_RAW) {
						if (coax_nav_mode != SB_NAV_STOP) {
							reachNavState(SB_NAV_STOP, 0.5);
							ros::Duration(0.5).sleep(); // make sure CoaX is in SB_NAV_STOP mode
						}
						reachNavState(SB_NAV_RAW, 0.5);
					}
					// set initial trim
					if (COAX == 56) {
						roll_trim = 0.0285;
						pitch_trim = 0.0921;
					} else {
						roll_trim = 0;
						pitch_trim = 0;
					}
					// switch to start procedure
					CONTROL_MODE = CONTROL_START;
					FIRST_START = true;
				} else {
					ROS_INFO("Battery Low!!! (%f V) Start denied",battery_voltage);
					LOW_POWER_DETECTED = true;
					out.result = -1;
				}
			} else {
				ROS_INFO("Start can only be executed from mode CONTROL_LANDED");
				out.result = -1;
			}

			break;
		
		case 3:
			if ((CONTROL_MODE == CONTROL_GOTOPOS) || (CONTROL_MODE == CONTROL_TRAJECTORY)){
				CONTROL_MODE = CONTROL_HOVER;
			FIRST_HOVER = true;
			} else {
				ROS_INFO("Hover mode can only be reached manually from mode CONTROL_GOTOPOS or CONTROL_TRAJECTORY");
				out.result = -1;
			}
			break;
		
		case 4:
			if (CONTROL_MODE == CONTROL_HOVER){
				CONTROL_MODE = CONTROL_GOTOPOS;
				FIRST_GOTOPOS = true;
				gotopos_position[0] = target_pose[0];
				gotopos_position[1] = target_pose[1];
				gotopos_position[2] = target_pose[2];
				gotopos_orientation = target_pose[3];
			} else {
				ROS_INFO("Gotopos mode can only be reached manually from mode CONTROL_HOVER");
				out.result = -1;
			}
			break;
			
		case 5:
			if ((CONTROL_MODE == CONTROL_HOVER) || (CONTROL_MODE == CONTROL_GOTOPOS)){
				CONTROL_MODE = CONTROL_TRAJECTORY;
				FIRST_TRAJECTORY = true;
			} else {
				ROS_INFO("Trajectory mode can only be reached manually from mode CONTROL_HOVER or CONTROL_GOTOPOS");
				out.result = -1;
			}
			break;
		
		case 6:
			if ((CONTROL_MODE == CONTROL_HOVER) || (CONTROL_MODE == CONTROL_GOTOPOS)){
				CONTROL_MODE = CONTROL_LANDING;
				FIRST_LANDING = true;
			} else {
				ROS_INFO("Landing mode can only be reached manually from mode CONTROL_HOVER or CONTROL_GOTOPOS");
				out.result = -1;
			}
			break;
		
		case 9:
			motor_up = 0;
			motor_lo = 0;
			servo_roll = 0;
			servo_pitch = 0;
			roll_trim = 0;
			pitch_trim = 0;
			
			reachNavState(SB_NAV_STOP, 0.5);
			
			CONTROL_MODE = CONTROL_LANDED;
			break;
		
		default:
			ROS_INFO("Non existent control mode request!");
			out.result = -1;
	}
	
	return true;
}

bool CoaxRosControl::setTrajectoryType(coax_ros_control::SetTrajectoryType::Request &req, coax_ros_control::SetTrajectoryType::Response &out)
{
	
	if ((req.trajectory_type <= 6) && (req.trajectory_type >= 0)) {
		TRAJECTORY_TYPE = req.trajectory_type;
		out.result = 0;
	} else {
		ROS_INFO("Non existent trajectory type!");
		out.result = -1;
	}
	
	return true;
}

bool CoaxRosControl::setTargetPose(coax_ros_control::SetTargetPose::Request &req, coax_ros_control::SetTargetPose::Response &out)
{

	if (req.x < -2) {
		target_pose[0] = -2;
	} else if (req.x > 2) {
		target_pose[0] = 2;
	} else {
		target_pose[0] = req.x;
	}
	if (req.y < -2) {
		target_pose[1] = -2;
	} else if (req.y > 2) {
		target_pose[1] = 2;
	} else {
		target_pose[1] = req.y;
	}
	if (req.z < 0.1) {
		target_pose[2] = 0.1;
	} else if (req.z > 4) {
		target_pose[2] = 4;
	} else {
		target_pose[2] = req.z;
	}
	target_pose[3] = req.orientation;
	
	while (target_pose[3] > M_PI) {
		target_pose[3] -= 2*M_PI;
	}
	while (target_pose[3] < -M_PI) {
		target_pose[3] += 2*M_PI;
	}
	
	return true;
}

//===================
// Parameter Setting
//===================

void CoaxRosControl::SetPlatform(int CoaX)
{
	COAX = CoaX;
}

void CoaxRosControl::SetMass(double mass)
{
	model_params.mass = mass;
}

void CoaxRosControl::SetInertia(double Ixx, double Iyy, double Izz)
{
	model_params.Ixx = Ixx;
	model_params.Iyy = Iyy;
	model_params.Izz = Izz;
}

void CoaxRosControl::SetRotorOffset(double d_up, double d_lo)
{
	model_params.d_up = d_up;
	model_params.d_lo = d_lo;
}

void CoaxRosControl::SetUpperRotorFollowingTime(double Tf_up)
{
	model_params.Tf_up = Tf_up;
}

void CoaxRosControl::SetRotorLinkageFactor(double l_up, double l_lo)
{
	model_params.l_up = l_up;
	model_params.l_lo = l_lo;
}

void CoaxRosControl::SetRotorSpringConstant(double k_springup, double k_springlo)
{
	model_params.k_springup = k_springup;
	model_params.k_springlo = k_springlo;
}

void CoaxRosControl::SetRotorThrustFactor(double k_Tup, double k_Tlo)
{
	model_params.k_Tup = k_Tup;
	model_params.k_Tlo = k_Tlo;
}

void CoaxRosControl::SetRotorMomentFactor(double k_Mup, double k_Mlo)
{
	model_params.k_Mup = k_Mup;
	model_params.k_Mlo = k_Mlo;
}

void CoaxRosControl::SetMotorFollowingTime(double Tf_motup, double Tf_motlo)
{
	model_params.Tf_motup = Tf_motup;
	model_params.Tf_motlo = Tf_motlo;
}

void CoaxRosControl::SetUpperRotorSpeedConversion(double rs_mup, double rs_bup)
{
	model_params.rs_mup = rs_mup;
	model_params.rs_bup = rs_bup;
}

void CoaxRosControl::SetLowerRotorSpeedConversion(double rs_mlo, double rs_blo)
{
	model_params.rs_mlo = rs_mlo;
	model_params.rs_blo = rs_blo;
}

void CoaxRosControl::SetUpperPhaseLag(double zeta_mup, double zeta_bup)
{
	model_params.zeta_mup = zeta_mup;
	model_params.zeta_bup = zeta_bup;
}

void CoaxRosControl::SetLowerPhaseLag(double zeta_mlo, double zeta_blo)
{
	model_params.zeta_mlo = zeta_mlo;
	model_params.zeta_blo = zeta_blo;
}

void CoaxRosControl::SetMaximumSwashPlateAngle(double max_SPangle)
{
	model_params.max_SPangle = max_SPangle;
}

void CoaxRosControl::SetHeaveYawGains(double Kp_Fz, double Kd_Fz, double Kp_Mz, double Kd_Mz)
{
	control_params.Kp_Fz = Kp_Fz;
	control_params.Kd_Fz = Kd_Fz;
	control_params.Kp_Mz = Kp_Mz;
	control_params.Kd_Mz = Kd_Mz;
}

void CoaxRosControl::SetLateralGains(double Kp_Fx, double Kp_Fy, double Kd_Fx, double Kd_Fy, double Kpq_roll, double Kpq_pitch)
{
	control_params.Kp_Fx = Kp_Fx;
	control_params.Kp_Fy = Kp_Fy;
	control_params.Kd_Fx = Kd_Fx;
	control_params.Kd_Fy = Kd_Fy;
	control_params.Kpq_roll = Kpq_roll;
	control_params.Kpq_pitch = Kpq_pitch;
}

void CoaxRosControl::load_model_params(ros::NodeHandle &n)
{
	
	double m;
	n.getParam("mass", m);
	SetMass(m);
	
	double Ixx, Iyy, Izz;
	n.getParam("inertia/Ixx", Ixx);
	n.getParam("inertia/Iyy", Iyy);
	n.getParam("inertia/Izz", Izz);
	SetInertia(Ixx, Iyy, Izz);
	
	double d_up, d_lo;
	n.getParam("offset/upper", d_up);
	n.getParam("offset/lower", d_lo);
	SetRotorOffset(d_up, d_lo);
	
	double l_up, l_lo;
	n.getParam("linkage_factor/upper", l_up);
	n.getParam("linkage_factor/lower", l_lo);
	SetRotorLinkageFactor(l_up, l_lo);
	
	double k_springup, k_springlo;
	n.getParam("spring_constant/upper", k_springup);
	n.getParam("spring_constant/lower", k_springlo);
	SetRotorSpringConstant(k_springup, k_springlo);
	
	double k_Tup, k_Tlo;
	n.getParam("thrust_factor/upper", k_Tup);
	n.getParam("thrust_factor/lower", k_Tlo);
	SetRotorThrustFactor(k_Tup, k_Tlo);
	
	double k_Mup, k_Mlo;
	n.getParam("moment_factor/upper", k_Mup);
	n.getParam("moment_factor/lower", k_Mlo);
	SetRotorMomentFactor(k_Mup, k_Mlo);
	
	double Tf_up;
	n.getParam("following_time/bar", Tf_up);
	SetUpperRotorFollowingTime(Tf_up);
	
	double Tf_motup, Tf_motlo;
	n.getParam("following_time/motors/upper", Tf_motup);
	n.getParam("following_time/motors/lower", Tf_motlo);
	SetMotorFollowingTime(Tf_motup, Tf_motlo);
	
	double rs_mup, rs_bup;
	n.getParam("speed_conversion/slope/upper", rs_mup);
	n.getParam("speed_conversion/offset/upper", rs_bup);
	SetUpperRotorSpeedConversion(rs_mup, rs_bup);
	
	double rs_mlo, rs_blo;
	n.getParam("speed_conversion/slope/lower", rs_mlo);
	n.getParam("speed_conversion/offset/lower", rs_blo);
	SetLowerRotorSpeedConversion(rs_mlo, rs_blo);
	
	double zeta_mup, zeta_bup;
	n.getParam("phase_lag/slope/upper", zeta_mup);
	n.getParam("phase_lag/offset/upper", zeta_bup);
	SetUpperPhaseLag(zeta_mup, zeta_bup);
	
	double zeta_mlo, zeta_blo;
	n.getParam("phase_lag/slope/lower", zeta_mlo);
	n.getParam("phase_lag/offset/lower", zeta_blo);
	SetLowerPhaseLag(zeta_mlo, zeta_blo);
	
	double max_SPangle;
	n.getParam("max_swashplate_angle", max_SPangle);
	SetMaximumSwashPlateAngle(max_SPangle);
	
	return;
}

void CoaxRosControl::load_control_params(ros::NodeHandle &n)
{
	double Kp_Fx;
	double Kp_Fy;
	double Kp_Fz;
	double Kd_Fx;
	double Kd_Fy;
	double Kd_Fz;
	double Kp_Mz;
	double Kd_Mz;
	double Kpq_roll;
	double Kpq_pitch;
	n.getParam("lateral/proportional/x",Kp_Fx);
	n.getParam("lateral/proportional/y",Kp_Fy);
	n.getParam("heave_yaw/force/proportional",Kp_Fz);
	n.getParam("lateral/differential/x",Kd_Fx);
	n.getParam("lateral/differential/y",Kd_Fy);
	n.getParam("heave_yaw/force/differential",Kd_Fz);
	n.getParam("heave_yaw/moment/proportional",Kp_Mz);
	n.getParam("heave_yaw/moment/differential",Kd_Mz);
	n.getParam("pq_damping/roll",Kpq_roll);
	n.getParam("pq_damping/pitch",Kpq_pitch);
	SetHeaveYawGains(Kp_Fz, Kd_Fz, Kp_Mz, Kd_Mz);
	SetLateralGains(Kp_Fx, Kp_Fy, Kd_Fx, Kd_Fy, Kpq_roll, Kpq_pitch);
}



int main(int argc, char** argv)
{
	ros::init(argc, argv, "coax_ros_control");
	
	ros::NodeHandle n("/coax_ros_control");
	
	CoaxRosControl api(n);
	
	ros::Duration(1.5).sleep(); // make sure coax_server has enough time to boot up
	api.configureComm(100, SBS_MODES | SBS_BATTERY | SBS_GYRO); // configuration of sending back data from CoaX
	api.setTimeout(500, 5000);
	
	int CoaX;
	n.param("CoaX", CoaX, 56);
	api.SetPlatform(CoaX);
	
	api.load_model_params(n);
	
	api.load_control_params(n);
	
	int frequency;
	n.param("frequency", frequency, 100);
	
	api.rawControlPublisher(frequency);

	return(0);
}
