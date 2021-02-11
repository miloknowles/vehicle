#pragma once

#include <string>
#include <functional>
#include <vector>

#include "core/cv_types.hpp"
#include "core/stereo_image.hpp"
#include "core/eigen_types.hpp"
#include "core/imu_measurement.hpp"
#include "core/timestamp.hpp"
#include "core/uid.hpp"

namespace bm {
namespace dataset {

using namespace core;

// Callback function signatures.
typedef std::function<void(StereoImage)> StereoCallback;
typedef std::function<void(ImuMeasurement)> ImuCallback;


struct SavedStereoData{
  SavedStereoData(timestamp_t timestamp,
                  const std::string& path_left,
                  const std::string& path_right)
      : timestamp(timestamp), path_left(path_left), path_right(path_right) {}

  timestamp_t timestamp;
  std::string path_left;
  std::string path_right;
};


class DataProvider {
 public:
  DataProvider() = default;

  void RegisterStereoCallback(StereoCallback cb) { stereo_callbacks_.emplace_back(cb); }
  void RegisterImuCallback(ImuCallback cb) { imu_callbacks_.emplace_back(cb); }

  // Retrieve ONE piece of data from whichever data source occurs next chronologically.
  // If there is a tie between different sources, prioritizes (1) IMU, (2) APS, (3) STEREO.
  bool Step(bool verbose = false);

  // Plays back all available data in real time, chronologically. Optionally changes the speed of
  // playback based on the factor "speed". If speed is < 0, returns data as fast as possible.
  void Playback(float speed, bool verbose = false);

 private:
  // Does sanity-checking on input data. Should be called before playback.
  void Validate() const;

  std::vector<StereoCallback> stereo_callbacks_;
  std::vector<ImuCallback> imu_callbacks_;

  // Timestamp of the last data item that was passed to a callback.
  timestamp_t last_data_timestamp_ = 0;

  // Stores current indices into the various data sources.
  size_t next_stereo_idx_ = 0;
  size_t next_imu_idx_ = 0;

 public:
  std::vector<SavedStereoData> stereo_data;
  std::vector<ImuMeasurement> imu_data;
};

}
}
