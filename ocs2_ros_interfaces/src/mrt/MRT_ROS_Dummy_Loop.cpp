/******************************************************************************
Copyright (c) 2020, Farbod Farshidian. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include "ocs2_ros_interfaces/mrt/MRT_ROS_Dummy_Loop.h"

namespace ocs2 {

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
MRT_ROS_Dummy_Loop::MRT_ROS_Dummy_Loop(MRT_ROS_Interface &mrt,
                                       scalar_t mrtDesiredFrequency,
                                       scalar_t mpcDesiredFrequency)
    : mrt_(mrt), mrtDesiredFrequency_(mrtDesiredFrequency),
      mpcDesiredFrequency_(mpcDesiredFrequency) {
  if (mrtDesiredFrequency_ < 0) {
    throw std::runtime_error("MRT loop frequency should be a positive number.");
  }

  if (mpcDesiredFrequency_ > 0) {
    ROS_WARN_STREAM("MPC loop is not realtime! For realtime setting, set "
                    "mpcDesiredFrequency to any negative number.");
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void MRT_ROS_Dummy_Loop::run(const SystemObservation &initObservation,
                             const TargetTrajectories &initTargetTrajectories) {
  ROS_INFO_STREAM("Waiting for the initial policy ...");

  // Reset MPC node
  mrt_.resetMpcNode(initTargetTrajectories);
  // [zmh]
  // 这里会调用/double_integrator_mpc_reset重置MPC的服务，并阻塞等待重置完成

  // Wait for the initial policy
  // [zmh] 这里在等待/double_integrator_mpc_policy话题传回第一帧的policy
  while (!mrt_.initialPolicyReceived() && ros::ok() && ros::master::check()) {
    mrt_.spinMRT();
    mrt_.setCurrentObservation(initObservation);
    ros::Rate(mrtDesiredFrequency_).sleep();
  }
  ROS_INFO_STREAM("Initial policy has been received.");

  // Pick simulation loop mode
  if (mpcDesiredFrequency_ > 0.0) {
    synchronizedDummyLoop(initObservation, initTargetTrajectories);
  } else {
    realtimeDummyLoop(initObservation, initTargetTrajectories);
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void MRT_ROS_Dummy_Loop::synchronizedDummyLoop(
    const SystemObservation &initObservation,
    const TargetTrajectories &initTargetTrajectories) {
  // Determine the ratio between MPC updates and simulation steps.
  const auto mpcUpdateRatio =
      std::max(static_cast<size_t>(mrtDesiredFrequency_ / mpcDesiredFrequency_),
               size_t(1));

  // Loop variables
  size_t loopCounter = 0;
  SystemObservation currentObservation = initObservation;

  // Helper function to check if policy is updated and starts at the given time.
  // Due to ROS message conversion delay and very fast MPC loop, we might get an
  // old policy instead of the latest one.
  const auto policyUpdatedForTime = [this](scalar_t time) {
    // policy must start within this fraction of dt
    constexpr scalar_t tol = 0.1;
    // [zmh] updatePolicy当mpc求解出新的policy时，这里返回的是true，反之
    return mrt_.updatePolicy() &&
           std::abs(mrt_.getPolicy().timeTrajectory_.front() - time) <
               (tol / mpcDesiredFrequency_);
  };

  ros::Rate simRate(mrtDesiredFrequency_);
  while (ros::ok() && ros::master::check()) {
    std::cout << "### Current time " << currentObservation.time << "\n";

    // Trigger MRT callbacks
    // [zmh] 这里手动spin，来处理/double_integrator_mpc_policy话题的回调函数，
    // 从而从mpc节点获取最新的policy并加入到缓存中
    mrt_.spinMRT();

    // Update the MPC policy if it is time to do so
    if (loopCounter % mpcUpdateRatio == 0) {
      // Wait for the policy to be updated
      // [zmh] 假设mrt为400Hz, mpc为100Hz, 那么每4次就应该等待更新一次policy,
      // 所以如果policy没有及时更新的话，会阻塞在这，导致observation和joint_state
      // 的发布频率降低
      while (!policyUpdatedForTime(currentObservation.time) && ros::ok() &&
             ros::master::check()) {
        mrt_.spinMRT();
      }
      std::cout << "<<< New MPC policy starting at "
                << mrt_.getPolicy().timeTrajectory_.front() << "\n";

      // // print the policy for debugging
      // const auto &policy = mrt_.getPolicy();
      // const auto &command = mrt_.getCommand();
      // std::cout << "======================new policy=====================" << "\n";
      // std::cout << "Time trajectory, size: "<<policy.timeTrajectory_.size()<<"\n";
      // for (const auto &t : policy.timeTrajectory_) {
      //   std::cout << t << " ";
      // }
      // std::cout << "\nState trajectory, size: "<< policy.stateTrajectory_.size()<< "\n";
      // for (const auto &state : policy.stateTrajectory_) {
      //   std::cout << state.transpose() << "\n";
      // }
      // std::cout << "Input trajectory, size: "<<policy.inputTrajectory_.size()<<"\n";
      // for (const auto &input : policy.inputTrajectory_) {
      //   std::cout << input.transpose() << "\n";
      // }
      // std::cout << "=====================================================" <<std::endl;

      // std::cout<<"~~~~~~~~~~~~~~~~~~~~~~~~~command target~~~~~~~~~~~~~~~~~" <<std::endl;
      // std::cout<< "timeTrajectory, size: "<<command.mpcTargetTrajectories_.timeTrajectory.size()<<"\n";
      // for (const auto &t : command.mpcTargetTrajectories_.timeTrajectory) {
      //   std::cout << t << " ";
      // }
      // std::cout << "\nState trajectory, size: "<< command.mpcTargetTrajectories_.stateTrajectory.size()<< "\n";
      // for (const auto &state : command.mpcTargetTrajectories_.stateTrajectory) {
      //   std::cout << state.transpose() << "\n";
      // }
      // std::cout << "Input trajectory, size: "<<command.mpcTargetTrajectories_.inputTrajectory.size()<<"\n";
      // for (const auto &input : command.mpcTargetTrajectories_.inputTrajectory) {
      //   std::cout << input.transpose() << "\n";
      // }
      // std::cout << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" <<std::endl;
    }
    // [zmh]
    // policyUpdatedForTime函数中对调用mrt_.updatePolicy()，这样就从缓存中获取了最新的mpc
    // policy, 后续通过mrt_.getPolicy()来获取最新的mpc policy --- PrimalSolution

    // Forward simulation
    // [zmh] 这里有两种方式进行observation的更新：
    // 1. 通过指定的控制器进行rollout
    // 2. 直接state和input进行插值
    currentObservation = forwardSimulation(currentObservation);

    // User-defined modifications before publishing
    modifyObservation(currentObservation);

    // Publish observation if at the next step we want a new policy
    if ((loopCounter + 1) % mpcUpdateRatio == 0) {
      // [zmh]
      // 这里进行observation更新后，会通知/double_integrator_mpc_observation话题的
      // 发布线程去发布最新的observation
      mrt_.setCurrentObservation(currentObservation);
      std::cout << ">>> Observation is published at " << currentObservation.time
                << "\n";
    }

    // Update observers
    for (auto &observer : observers_) {
      // [zmh]
      // 这里将根据最新的observation和policy来更新/joint_states话题的发布命令，使rviz中的模型动起来
      observer->update(currentObservation, mrt_.getPolicy(), mrt_.getCommand());
    }

    ++loopCounter;
    ros::spinOnce();
    simRate.sleep();
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void MRT_ROS_Dummy_Loop::realtimeDummyLoop(
    const SystemObservation &initObservation,
    const TargetTrajectories &initTargetTrajectories) {
  // Loop variables
  SystemObservation currentObservation = initObservation;

  ros::Rate simRate(mrtDesiredFrequency_);
  while (ros::ok() && ros::master::check()) {
    std::cout << "### Current time " << currentObservation.time << "\n";

    // Trigger MRT callbacks
    // [zmh] 处理下policy的回调函数，看是否有新的policy
    mrt_.spinMRT();

    // Update the policy if a new on was received
    if (mrt_.updatePolicy()) {
      std::cout << "<<< New MPC policy starting at "
                << mrt_.getPolicy().timeTrajectory_.front() << "\n";
    }

    // Forward simulation
    currentObservation = forwardSimulation(currentObservation);

    // User-defined modifications before publishing
    modifyObservation(currentObservation);

    // Publish observation
    mrt_.setCurrentObservation(currentObservation);

    // Update observers
    for (auto &observer : observers_) {
      observer->update(currentObservation, mrt_.getPolicy(), mrt_.getCommand());
    }

    ros::spinOnce();
    simRate.sleep();
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
SystemObservation MRT_ROS_Dummy_Loop::forwardSimulation(
    const SystemObservation &currentObservation) {
  const scalar_t dt = 1.0 / mrtDesiredFrequency_;

  SystemObservation nextObservation;
  nextObservation.time = currentObservation.time + dt;
  if (mrt_.isRolloutSet()) { // If available, use the provided rollout as to
                             // integrate the dynamics.
    mrt_.rolloutPolicy(currentObservation.time, currentObservation.state, dt,
                       nextObservation.state, nextObservation.input,
                       nextObservation.mode);
  } else { // Otherwise, we fake integration by interpolating the current MPC
           // policy at t+dt
    mrt_.evaluatePolicy(currentObservation.time + dt, currentObservation.state,
                        nextObservation.state, nextObservation.input,
                        nextObservation.mode);
  }

  return nextObservation;
}

} // namespace ocs2
