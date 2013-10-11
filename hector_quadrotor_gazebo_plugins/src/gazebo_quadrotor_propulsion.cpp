//=================================================================================================
// Copyright (c) 2012, Johannes Meyer, TU Darmstadt
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the Flight Systems and Automatic Control group,
//       TU Darmstadt, nor the names of its contributors may be used to
//       endorse or promote products derived from this software without
//       specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//=================================================================================================

#include <hector_quadrotor_gazebo_plugins/gazebo_quadrotor_propulsion.h>
#include <hector_quadrotor_model/helpers.h>

#include <gazebo/common/Events.hh>
#include <gazebo/physics/physics.hh>

#include <rosgraph_msgs/Clock.h>

namespace gazebo {

using namespace common;
using namespace math;
using namespace hector_quadrotor_model;

GazeboQuadrotorPropulsion::GazeboQuadrotorPropulsion()
{
}

GazeboQuadrotorPropulsion::~GazeboQuadrotorPropulsion()
{
  event::Events::DisconnectWorldUpdateBegin(updateConnection);
  node_handle_->shutdown();
  callback_queue_thread_.join();
  delete node_handle_;
}

////////////////////////////////////////////////////////////////////////////////
// Load the controller
void GazeboQuadrotorPropulsion::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
  world = _model->GetWorld();
  link = _model->GetLink();

  // default parameters
  namespace_.clear();
  param_namespace_ = "quadrotor_propulsion";
  trigger_topic_ = "quadro/trigger";
  voltage_topic_ = "motor_pwm";
  wrench_topic_ = "wrench_out";
  supply_topic_ = "supply";
  status_topic_ = "motor_status";
  control_tolerance_ = ros::Duration();
  control_delay_ = ros::Duration();

  // load parameters
  if (_sdf->HasElement("robotNamespace"))   namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();
  if (_sdf->HasElement("paramNamespace"))   param_namespace_ = _sdf->GetElement("paramNamespace")->Get<std::string>();
  if (_sdf->HasElement("triggerTopic"))     trigger_topic_ = _sdf->GetElement("triggerTopic")->Get<std::string>();
  if (_sdf->HasElement("voltageTopicName")) voltage_topic_ = _sdf->GetElement("voltageTopicName")->Get<std::string>();
  if (_sdf->HasElement("wrenchTopic"))      wrench_topic_ = _sdf->GetElement("wrenchTopic")->Get<std::string>();
  if (_sdf->HasElement("supplyTopic"))      supply_topic_ = _sdf->GetElement("supplyTopic")->Get<std::string>();
  if (_sdf->HasElement("statusTopic"))      status_topic_ = _sdf->GetElement("statusTopic")->Get<std::string>();

  // get model parameters
  model_.configure(param_namespace_);

  // set control timing parameters
  controlTimer.Load(world, _sdf, "control");
  if (_sdf->HasElement("controlTolerance")) control_tolerance_.fromSec(_sdf->GetElement("controlTolerance")->Get<double>());
  if (_sdf->HasElement("controlDelay"))     control_delay_.fromSec(_sdf->GetElement("controlDelay")->Get<double>());

  // set initial supply voltage
  if (_sdf->HasElement("supplyVoltage"))    model_.setInitialSupplyVoltage(_sdf->GetElement("supplyVoltage")->Get<double>());

  // start ros node
  if (!ros::isInitialized())
  {
    int argc = 0;
    char **argv = NULL;
    ros::init(argc,argv,"gazebo",ros::init_options::NoSigintHandler|ros::init_options::AnonymousName);
  }
  node_handle_ = new ros::NodeHandle(namespace_);

  // publish trigger
  if (!trigger_topic_.empty())
  {
    ros::AdvertiseOptions ops;
    ops.callback_queue = &callback_queue_;
    ops.init<rosgraph_msgs::Clock>(trigger_topic_, 10);
    trigger_publisher_ = node_handle_->advertise(ops);
  }

  // subscribe command
  if (!voltage_topic_.empty())
  {
    ros::SubscribeOptions ops;
    ops.callback_queue = &callback_queue_;
    ops.init<hector_uav_msgs::MotorPWM>(voltage_topic_, 1, boost::bind(&QuadrotorPropulsion::addVoltageToQueue, &model_, _1));
    voltage_subscriber_ = node_handle_->subscribe(ops);
  }

  // publish wrench
  if (!wrench_topic_.empty())
  {
    ros::AdvertiseOptions ops;
    ops.callback_queue = &callback_queue_;
    ops.init<geometry_msgs::Wrench>(wrench_topic_, 10);
    wrench_publisher_ = node_handle_->advertise(ops);
  }

  // publish and latch supply
  if (!supply_topic_.empty())
  {
    ros::AdvertiseOptions ops;
    ops.callback_queue = &callback_queue_;
    ops.latch = true;
    ops.init<hector_uav_msgs::Supply>(supply_topic_, 10);
    supply_publisher_ = node_handle_->advertise(ops);
    supply_publisher_.publish(model_.getSupply());
  }

  // publish motor_status
  if (!status_topic_.empty())
  {
    ros::AdvertiseOptions ops;
    ops.callback_queue = &callback_queue_;
    ops.init<hector_uav_msgs::MotorStatus>(status_topic_, 10);
    motor_status_publisher_ = node_handle_->advertise(ops);
  }

  // callback_queue_thread_ = boost::thread( boost::bind( &GazeboQuadrotorPropulsion::QueueThread,this ) );

  Reset();

  // New Mechanism for Updating every World Cycle
  // Listen to the update event. This event is broadcast every
  // simulation iteration.
  updateConnection = event::Events::ConnectWorldUpdateBegin(
      boost::bind(&GazeboQuadrotorPropulsion::Update, this));
}

////////////////////////////////////////////////////////////////////////////////
// Update the controller
void GazeboQuadrotorPropulsion::Update()
{
  // Get simulator time
  Time current_time = world->GetSimTime();
  Time dt = current_time - last_time_;
  last_time_ = current_time;
  if (dt <= 0.0) return;

  // Send trigger
  bool trigger = controlTimer.update();
  if (trigger && trigger_publisher_) {
    rosgraph_msgs::Clock clock;
    clock.clock = ros::Time(current_time.sec, current_time.nsec);
    trigger_publisher_.publish(clock);

    ROS_DEBUG_STREAM_NAMED("quadrotor_propulsion", "Sent a trigger message at t = " << current_time.Double() << " (dt = " << (current_time - last_trigger_time_).Double() << ")");
    last_trigger_time_ = current_time;
  }

  // Get new commands/state
  callback_queue_.callAvailable();

  // Process input queue
  model_.processQueue(ros::Time(current_time.sec, current_time.nsec), control_tolerance_, control_delay_, (model_.getMotorStatus().on && trigger) ? ros::WallDuration(1.0) : ros::WallDuration(), &callback_queue_);

  // fill input vector u for propulsion model
  geometry_msgs::Twist twist;
  fromVector(link->GetRelativeLinearVel(), twist.linear);
  fromVector(link->GetRelativeAngularVel(), twist.angular);
  model_.setTwist(twist);

  // update the model
  model_.update(dt.Double());

  // get wrench from model
  Vector3 force, torque;
  toVector(model_.getWrench().force, force);
  toVector(model_.getWrench().torque, torque);

  // publish wrench
  if (wrench_publisher_) {
    wrench_publisher_.publish(model_.getWrench());
  }

  // publish motor status
  if (motor_status_publisher_ && trigger /* && current_time >= last_motor_status_time_ + control_period_ */) {
    hector_uav_msgs::MotorStatus motor_status = model_.getMotorStatus();
    motor_status.header.stamp = ros::Time(current_time.sec, current_time.nsec);
    motor_status_publisher_.publish(motor_status);
    last_motor_status_time_ = current_time;
  }

  // publish supply
  if (supply_publisher_ && current_time >= last_supply_time_ + 1.0) {
    supply_publisher_.publish(model_.getSupply());
    last_supply_time_ = current_time;
  }

  // set force and torque in gazebo
  link->AddRelativeForce(force);
  link->AddRelativeTorque(torque - link->GetInertial()->GetCoG().Cross(force));
}

////////////////////////////////////////////////////////////////////////////////
// Reset the controller
void GazeboQuadrotorPropulsion::Reset()
{
  model_.reset();
  last_time_ = Time();
  last_trigger_time_ = Time();
  last_motor_status_time_ = Time();
  last_supply_time_ = Time();
}

////////////////////////////////////////////////////////////////////////////////
// custom callback queue thread
void GazeboQuadrotorPropulsion::QueueThread()
{
  static const double timeout = 0.01;

  while (node_handle_->ok())
  {
    callback_queue_.callAvailable(ros::WallDuration(timeout));
  }
}

// Register this plugin with the simulator
GZ_REGISTER_MODEL_PLUGIN(GazeboQuadrotorPropulsion)

} // namespace gazebo