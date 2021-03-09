#include "vio/imu_manager.hpp"

namespace bm {
namespace vio {


ImuManager::ImuManager(const Params& params)
    : params_(params),
      queue_(params_.max_queue_size, true)
{
  // https://github.com/haidai/gtsam/blob/master/examples/ImuFactorsExample.cpp
  const gtsam::Matrix3 measured_acc_cov = gtsam::I_3x3 * std::pow(params_.accel_noise_sigma, 2);
  const gtsam::Matrix3 measured_omega_cov = gtsam::I_3x3 * std::pow(params_.gyro_noise_sigma, 2);
  const gtsam::Matrix3 integration_error_cov = gtsam::I_3x3 * 1e-8;
  const gtsam::Matrix3 bias_acc_cov = gtsam::I_3x3 * std::pow(params_.accel_bias_rw_sigma, 2);
  const gtsam::Matrix3 bias_omega_cov = gtsam::I_3x3 * std::pow(params_.gyro_bias_rw_sigma, 2);
  const gtsam::Matrix6 bias_acc_omega_int = gtsam::I_6x6 * 1e-5;

  // Set up all of the params for preintegration.
  pim_params_ = PimC::Params(params_.n_gravity);
  pim_params_.setBiasAccOmegaInt(bias_acc_omega_int);
  pim_params_.setAccelerometerCovariance(measured_acc_cov);
  pim_params_.setGyroscopeCovariance(measured_omega_cov);
  pim_params_.setIntegrationCovariance(integration_error_cov);
  pim_params_.setBiasAccCovariance(bias_acc_cov);
  pim_params_.setBiasOmegaCovariance(bias_omega_cov);
  pim_params_.print();

  pim_ = PimC(boost::make_shared<PimC::Params>(pim_params_)); // Initialize with zero bias.
}


void ImuManager::Push(const ImuMeasurement& imu)
{
  // TODO(milo): Eventually deal with dropped IMU measurements (preintegrate them out).
  queue_.Push(std::move(imu));
}


PimResult ImuManager::Preintegrate(seconds_t from_time, seconds_t to_time)
{
  // If no measurements, return failure.
  if (queue_.Empty()) {
    return PimResult(false, kMinSeconds, kMaxSeconds, PimC());
  }

  // Get the first measurement >= from_time.
  ImuMeasurement imu = queue_.Pop();
  while (ConvertToSeconds(queue_.PeekFront().timestamp) <= from_time) {
    imu = queue_.Pop();
  }

  const double earliest_imu_sec = ConvertToSeconds(imu.timestamp);

  // FAIL: No measurement close to (specified) from_time.
  const double offset_from_sec = (from_time != kMinSeconds) ? std::fabs(earliest_imu_sec - from_time) : 0.0;
  if (offset_from_sec > params_.allowed_misalignment_sec) {
    return PimResult(false, kMinSeconds, kMaxSeconds, PimC());
  }

  // Assume CONSTANT acceleration between from_time and nearest IMU measurement.
  // https://github.com/borglab/gtsam/blob/develop/gtsam/navigation/CombinedImuFactor.cpp
  // NOTE(milo): There is a divide by dt in the source code.
  if (offset_from_sec > 0) {
    pim_.integrateMeasurement(imu.a, imu.w, offset_from_sec);
  }

  // Integrate all measurements < to_time.
  double last_imu_time_sec = earliest_imu_sec;
  while (!queue_.Empty() && ConvertToSeconds(queue_.PeekFront().timestamp) < to_time) {
    imu = queue_.Pop();
    const double dt = ConvertToSeconds(imu.timestamp) - last_imu_time_sec;
    if (dt > 0) { pim_.integrateMeasurement(imu.a, imu.w, dt); }
    last_imu_time_sec = ConvertToSeconds(imu.timestamp);
  }

  const double latest_imu_sec = ConvertToSeconds(imu.timestamp);

  // FAIL: No measurement close to (specified) to_time.
  const double offset_to_sec = (to_time != kMaxSeconds) ? std::fabs(latest_imu_sec - to_time) : 0.0;
  if (offset_to_sec > params_.allowed_misalignment_sec) {
    pim_.resetIntegration();
    return PimResult(false, kMinSeconds, kMaxSeconds, PimC());
  }

  // Assume CONSTANT acceleration between to_time and nearest IMU measurement.
  if (offset_to_sec > 0) {
    pim_.integrateMeasurement(imu.a, imu.w, offset_to_sec);
  }

  const PimResult out = PimResult(true, earliest_imu_sec, latest_imu_sec, pim_);
  pim_.resetIntegration();

  return out;
}


void ImuManager::ResetAndUpdateBias(const ImuBias& bias)
{
  pim_.resetIntegrationAndSetBias(bias);
}


void ImuManager::DiscardBefore(seconds_t time)
{
  while (!queue_.Empty() && queue_.PeekFront().timestamp < time) {
    queue_.Pop();
  }
}


}
}
