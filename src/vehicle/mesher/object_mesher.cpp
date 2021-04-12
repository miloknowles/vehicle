#include <glog/logging.h>

#include <boost/graph/connected_components.hpp>

#include <opencv2/imgproc.hpp>

#include "core/timer.hpp"
#include "core/image_util.hpp"
#include "core/math_util.hpp"
#include "core/color_mapping.hpp"
#include "feature_tracking/visualization_2d.hpp"
#include "mesher/neighbor_grid.hpp"
#include "mesher/object_mesher.hpp"

namespace bm {
namespace mesher {


void EstimateForegroundMask(const Image1b& gray,
                            Image1b& mask,
                            int ksize,
                            double min_grad,
                            int downsize)
{
  CHECK(downsize >= 1 && downsize <= 8) << "Use a downsize argument (int) between 1 and 8" << std::endl;
  const int scaled_ksize = ksize / downsize;
  CHECK_GT(scaled_ksize, 1) << "ksize too small for downsize" << std::endl;
  const int kwidth = 2*scaled_ksize + 1;

  const cv::Mat kernel = cv::getStructuringElement(
      cv::MORPH_RECT,
      cv::Size(kwidth, kwidth),
      cv::Point(scaled_ksize, scaled_ksize));

  // Do image processing at a downsampled size (faster).
  if (downsize > 1) {
    Image1b gray_small;
    cv::resize(gray, gray_small, gray.size() / downsize, 0, 0, cv::INTER_LINEAR);
    cv::Mat gradient;
    cv::morphologyEx(gray_small, gradient, cv::MORPH_GRADIENT, kernel, cv::Point(-1, -1), 1);
    cv::resize(gradient > min_grad, mask, gray.size(), 0, 0, cv::INTER_LINEAR);

  // Do processing at original resolution.
  } else {
    cv::Mat gradient;
    cv::morphologyEx(gray, gradient, cv::MORPH_GRADIENT, kernel, cv::Point(-1, -1), 1);
    mask = gradient > min_grad;
  }
}


void DrawDelaunay(int k,
                  Image3b& img,
                  cv::Subdiv2D& subdiv,
                  const CoordinateMap<int>& vertex_lookup,
                  const CoordinateMap<double>& vertex_disps,
                  double min_disp = 0.5,
                  double max_disp = 32.0)
{
  std::vector<cv::Vec6f> triangle_list;
  subdiv.getTriangleList(triangle_list);
  std::vector<cv::Point> pt(3);

  const cv::Size size = img.size();
  cv::Rect rect(0,0, size.width, size.height);

  const double disp_range = max_disp - min_disp;

  for (size_t i = 0; i < triangle_list.size(); ++i) {
    const cv::Vec6f& t = triangle_list.at(i);
    pt[0] = cv::Point(t[0], t[1]);
    pt[1] = cv::Point(t[2], t[3]);
    pt[2] = cv::Point(t[4], t[5]);

    if (rect.contains(pt[0]) && rect.contains(pt[1]) && rect.contains(pt[2])) {
      const int v0 = vertex_lookup.At((int)pt[0].x, (int)pt[0].y);
      const int v1 = vertex_lookup.At((int)pt[1].x, (int)pt[1].y);
      const int v2 = vertex_lookup.At((int)pt[2].x, (int)pt[2].y);

      const std::vector<double> disps = {
        0.5*vertex_disps.At(k, v0) + 0.5*vertex_disps.At(k, v1),
        0.5*vertex_disps.At(k, v1) + 0.5*vertex_disps.At(k, v2),
        0.5*vertex_disps.At(k, v2) + 0.5*vertex_disps.At(k, v0)
      };

      const std::vector<cv::Vec3b> colors = ColormapVector(disps, 0.5, 32.0, cv::COLORMAP_PARULA);
      cv::line(img, pt[0], pt[1], colors.at(0), 1, CV_AA, 0);
	    cv::line(img, pt[1], pt[2], colors.at(1), 1, CV_AA, 0);
	    cv::line(img, pt[2], pt[0], colors.at(2), 1, CV_AA, 0);
    }
  }
}


static void CountEdgePixels(const cv::Point2f& a,
                            const cv::Point2f& b,
                            const Image1b& mask,
                            int& edge_sum,
                            int& edge_length)
{
  edge_sum = 0;

  cv::LineIterator it(mask, a, b, 8, false);
  edge_length = it.count;

  for (int i = 0; i < it.count; ++i, ++it) {
    const uint8_t v = mask.at<uint8_t>(it.pos());
    edge_sum += v > 0 ? 1 : 0;
  }
}


void ObjectMesher::ProcessStereo(const StereoImage1b& stereo_pair)
{
  const Image1b& iml = stereo_pair.left_image;

  Timer timer(true);
  tracker_.TrackAndTriangulate(stereo_pair, false);
  // LOG(INFO) << "TrackAndTriangulate: " << timer.Tock().milliseconds() << std::endl;

  const Image3b& viz_tracks = tracker_.VisualizeFeatureTracks();

  cv::imshow("Feature Tracks", viz_tracks);

  Image1b foreground_mask;
  EstimateForegroundMask(iml, foreground_mask, params_.foreground_ksize, params_.foreground_min_gradient, 4);

  cv::imshow("Foreground Mask", foreground_mask);

  // Build a keypoint graph.
  std::vector<uid_t> lmk_ids;
  std::vector<cv::Point2f> lmk_points;
  std::vector<double> lmk_disps;

  const FeatureTracks& live_tracks = tracker_.GetLiveTracks();

  for (auto it = live_tracks.begin(); it != live_tracks.end(); ++it) {
    const uid_t lmk_id = it->first;
    const LandmarkObservation& lmk_obs = it->second.back();

    // Skip observations from previous frames.
    if (lmk_obs.camera_id < (stereo_pair.camera_id - params_.tracker_params.retrack_frames_k)) {
      continue;
    }
    lmk_points.emplace_back(lmk_obs.pixel_location);
    lmk_disps.emplace_back(lmk_obs.disparity);
    lmk_ids.emplace_back(lmk_id);
  }

  // Map all of the features into the coarse grid so that we can find NNs.
  lmk_grid_.Clear();
  const std::vector<Vector2i> lmk_cells = MapToGridCells(
      lmk_points, iml.rows, iml.cols, lmk_grid_.Rows(), lmk_grid_.Cols());

  timer.Reset();
  PopulateGrid(lmk_cells, lmk_grid_);

  LmkGraph graph;

  for (size_t i = 0; i < lmk_ids.size(); ++i) {
    // const uid_t lmk_id = lmk_ids.at(i);
    const Vector2i lmk_cell = lmk_cells.at(i);
    const core::Box2i roi(lmk_cell - Vector2i(1, 1), lmk_cell + Vector2i(1, 1));
    const std::list<uid_t>& roi_indices = lmk_grid_.GetRoi(roi);

    // Add a graph edge to all other landmarks nearby.
    for (uid_t j : roi_indices) {
      if (i == j) { continue; }

      // Only add edge if the vertices are within some 3D distance from each other.
      const double depth_i = params_.stereo_rig.DispToDepth(lmk_disps.at(i));
      const double depth_j = params_.stereo_rig.DispToDepth(lmk_disps.at(j));
      if (std::fabs(depth_i - depth_j) > params_.edge_max_depth_change) {
        continue;
      }

      // Only add an edge to the grab if it has texture (an object) underneath it.
      int edge_length = 0;
      int edge_sum = 0;
      CountEdgePixels(lmk_points.at(i), lmk_points.at(j), foreground_mask, edge_sum, edge_length);
      const float fgd_percent = static_cast<float>(edge_sum) / static_cast<float>(edge_length);
      if (fgd_percent < params_.edge_min_foreground_percent) {
        continue;
      }

      boost::add_edge(i, j, graph);
    }
  }

  if (boost::num_vertices(graph) > 0) {
    std::vector<int> assignments(boost::num_vertices(graph));

    timer.Reset();
    const int num_comp = boost::connected_components(graph, &assignments[0]);

    std::vector<int> nmembers(num_comp, 0);
    std::vector<cv::Subdiv2D> subdivs(num_comp, { cv::Rect(0, 0, iml.cols, iml.rows) });

    // std::unordered_map<int, uid_t> vertex_id_to_lmk_id;
    // std::unordered_map<int, double> vertex_disps;

    CoordinateMap<uid_t> vertex_id_to_lmk_id;
    CoordinateMap<double> vertex_disps;
    MultiCoordinateMap vertex_lookup;

    timer.Reset();
    for (size_t i = 0; i < assignments.size(); ++i) {
      const int cmp_id = assignments.at(i);
      const cv::Point2f lmk_pt = lmk_points.at(i);
      const int vertex_id = subdivs.at(cmp_id).insert(lmk_pt);
      vertex_lookup[cmp_id].Insert((int)lmk_pt.x, (int)lmk_pt.y, vertex_id);
      vertex_id_to_lmk_id.Insert(cmp_id, vertex_id, lmk_ids.at(i));
      vertex_disps.Insert(cmp_id, vertex_id, lmk_disps.at(i));

      ++nmembers.at(cmp_id);
    }

    // Draw the output triangles.
    cv::Mat3b viz_triangles;
    cv::cvtColor(iml, viz_triangles, cv::COLOR_GRAY2BGR);

    for (size_t k = 0; k < subdivs.size(); ++k) {
      // Ignore meshes without at least one triangle.
      if (nmembers.at(k) < 3) {
        continue;
      }
      DrawDelaunay(k, viz_triangles, subdivs.at(k), vertex_lookup.at(k), vertex_disps);
    }

    cv::imshow("delaunay", viz_triangles);
  }

  cv::waitKey(1);
}


}
}
