#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/make_shared.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <algorithm>
#include <fstream>
#include <cmath>

#include "kinect_view.h"

namespace kinect_merge {

// This is used for deciding whether two points are similar enough to be merged.
static const float SIMILARITY_THRESHOLD = 3.0f; // in units of standard deviations

// Measurement variance constants
static const float ALPHA_0 = 0.0032225f;
static const float ALPHA_1 = -0.0020925f;
static const float ALPHA_2 = 0.0022078f;
static const float BETA_X = 0.0017228f;
static const float BETA_Y = 0.0017092f;

CPoint::CPoint() {}

CPoint::CPoint(const cv::Matx31f& position, const cv::Matx33f& covariance, const cv::Matx<unsigned char, 3, 1>& color) :
    pos(position),
    cov(covariance),
    col_sum(color),
    num_merged(1) {}

cv::Matx<unsigned char, 3, 1> CPoint::get_color() const {
    // NOTE: The following line would segfault on cv::Mat(col_sum). An alternate method follows.
    // return cv::Mat(col_sum) / num_merged;
    cv::Matx<unsigned char, 3, 1> m;
    for(int i = 0; i < 3; i++)
        m(i) = col_sum(i) / num_merged;
    return m;
}

CView::CView(const std::string &extrinsic_file,
             const std::string &disparity_file,
             const std::string &color_file,
             float covariance_scale_xy,
             float covariance_scale_z,
             CKinectCalibration &calibration_) :
    calibration(calibration_),
    original_pointmap(boost::extents[IMAGE_HEIGHT][IMAGE_WIDTH]),
    pointmap(boost::extents[IMAGE_HEIGHT][IMAGE_WIDTH]),
    num_missing(0),
#ifdef DEBUG // The measurement_used vector is used for outputting debug images.
    num_added(0),
    measurement_used(IMAGE_HEIGHT) {
#else
    num_added(0) {
#endif

    // Initialize the depth map.
    for(int v = 0; v < IMAGE_HEIGHT; v++) {
        for(int u = 0; u < IMAGE_WIDTH; u++) {
            depthmap(v, u) = -1;
        }
    }

    // Load extrinsic data.

    cv::Mat inv_transformation_tmp = cv::Mat_<float>::eye(4, 4);
    cv::Mat inv_rotation(inv_transformation_tmp, cv::Rect(0, 0, 3, 3));
    cv::Mat inv_translation(inv_transformation_tmp, cv::Rect(3, 0, 1, 3));

    try {
        cv::FileStorage fs(extrinsic_file, cv::FileStorage::READ);
        fs["R"] >> inv_rotation;
        fs["T"] >> inv_translation;
    } catch(cv::Exception &e) {
        std::cerr << std::endl << "Error reading extrinsic data: " << e.what() << std::endl;
        exit(4);
    }

    inv_transformation = inv_transformation_tmp;
    cv::Mat transformation = inv_transformation_tmp.inv();
    cv::Mat rotation = inv_rotation.t();

    // Load disparity map.
    int flags = -1; // Load as-is (greyscale). Using 0 (force grayscale) results in a segfault.
    cv::Mat1s disparitymap = cv::imread(disparity_file, flags);

    if(disparitymap.cols != IMAGE_WIDTH || disparitymap.rows != IMAGE_HEIGHT) {
        std::cerr << std::endl << "Disparity map has dimensions " << disparitymap.cols << "x" << disparitymap.rows <<
                                  " instead of " << IMAGE_WIDTH << "x" << IMAGE_HEIGHT << std::endl;
        exit(5);
    }

    // Load color map.
    cv::Mat3b colormap = cv::imread(color_file, flags);

    if(colormap.cols != IMAGE_WIDTH || colormap.rows != IMAGE_HEIGHT) {
        std::cerr << std::endl << "Color map has dimensions " << colormap.cols << "x" << colormap.rows <<
                                  " instead of " << IMAGE_WIDTH << "x" << IMAGE_HEIGHT << std::endl;
        exit(6);
    }

    // Convert to a point cloud in global coordinates and calculate the covariance matrix.
    cv::Matx31f local_point;
    cv::Mat local_point_tmp = (cv::Mat_<float>(4, 1) << 0, 0, 0, 1);
    cv::Mat global_point;
    cv::Mat covariance;
    for(int v = 0; v < disparitymap.rows; v++) {
        for(int u = 0; u < disparitymap.cols; u++) {
            const short disparity = disparitymap(v, u);
            if(disparity == 2047) {
                continue;
            }

            // Convert to local and then global coordinates.
            calibration.disparity2point(u, v, disparity, local_point);
            local_point_tmp.at<float>(0) = local_point(0);
            local_point_tmp.at<float>(1) = local_point(1);
            local_point_tmp.at<float>(2) = local_point(2);
            global_point = transformation * local_point_tmp;
            global_point.pop_back(); // Turn into a 3 by 1 matrix.

            float depth = local_point(2);
            if(depth < 0) {
                // The point projects behind the camera. Ignore it.
                continue;
            }

            // Calculate the covariance matrix.
            covariance = cv::Mat_<float>::eye(3, 3);
            covariance.at<float>(0, 0) = BETA_X * BETA_X * depth * depth / 12;
            covariance.at<float>(1, 1) = BETA_Y * BETA_Y * depth * depth / 12;
            covariance.at<float>(2, 2) = pow(ALPHA_2 * depth * depth + ALPHA_1 * depth + ALPHA_0, 2);
            covariance.at<float>(0, 0) *= covariance_scale_xy;
            covariance.at<float>(1, 1) *= covariance_scale_xy;
            covariance.at<float>(2, 2) *= covariance_scale_z;

            // Convert the covariance matrix to the global reference frame.
            covariance = rotation * covariance * inv_rotation;
            assert(covariance.at<float>(0, 0) >= 0);
            assert(covariance.at<float>(1, 1) >= 0);
            assert(covariance.at<float>(2, 2) >= 0);

            // Get the pixel coordinates for the measurement.
            int uc, vc;
            to_image_plane(local_point_tmp, uc, vc);
            if(uc < 0 || uc >= IMAGE_WIDTH || vc < 0 || vc >= IMAGE_HEIGHT) {
                continue;
            }

            if(!original_pointmap[vc][uc] || depthmap(vc, uc) > depth) {
                depthmap(vc, uc) = depth;
                original_pointmap[vc][uc] = boost::make_shared<CPoint>(global_point,
                                                                       covariance,
                                                                       colormap(vc, uc));
            }
        }
    }
}

void CView::to_image_plane(const cv::Matx41f &local_pos, int &uc, int &vc) const {
    cv::Matx31f local_pos_tmp(local_pos(0), local_pos(1), local_pos(2));
    cv::Matx21f cf;
    calibration.point2rgb(local_pos_tmp, cf);
    uc = cf(0) + 0.5;
    vc = cf(1) + 0.5;
}

static float calculate_mahalanobis_distance(const CPoint &refined_point, const CPoint &p) {
    cv::Matx<float, 1, 1> operand = (p.pos - refined_point.pos).t() * p.cov.inv() * (p.pos - refined_point.pos);
    return sqrt(operand(0));
}

// Calculate a new position estimate, covariance matrix and color for an existing point given a new measurement.
static void refine_point(const CPoint &existing_point, const CPoint &new_measurement, CPoint &refined_point) {
    cv::Matx33f new_measurement_cov_inv = new_measurement.cov.inv();

    // Combine the covariance matrices.
    refined_point.cov = (existing_point.cov.inv() + new_measurement_cov_inv).inv();

    // Calculate the refined position estimate.
    refined_point.pos = existing_point.pos + refined_point.cov * new_measurement_cov_inv * (new_measurement.pos - existing_point.pos);

    // Add the color values.
    refined_point.col_sum = existing_point.col_sum + new_measurement.col_sum;
    refined_point.num_merged = existing_point.num_merged + new_measurement.num_merged;
}

void CView::refine_points(CView &connected_view, std::vector<std::vector<bool> > &measurement_used) const {
    for(int v = 0; v < IMAGE_HEIGHT; v++) {
        for(int u = 0; u < IMAGE_WIDTH; u++) {
            if(!connected_view.has_point(u, v)) {
                continue;
            }

            // Project the point onto the current view image plane.
            // NOTE: Only points which were added to cloud from the connected view are projected.
            // Each point in the cloud is pointed to by exactly one view. This means that a single
            // point in the cloud will NOT be refined multiple times with the same new measurement
            // through different connected views.
            int proj_u, proj_v;
            float proj_depth;
            CPoint &connected_view_point = connected_view.get_point(u, v);
            project(connected_view_point, proj_u, proj_v, proj_depth);

            if(proj_depth < 0) {
                // The point projects outside the image plane.
                continue;
            }

            if(!has_measurement(proj_u, proj_v)) {
                // There is nothing to merge with at this pixel.
                continue;
            }

            const CPoint &current_view_point = get_original_point(proj_u, proj_v);

            // Calculate the refined point and use it if it's close enough to the existing point and
            // new measurement.
            CPoint refined_point;
            refine_point(connected_view_point, current_view_point, refined_point);
            if(calculate_mahalanobis_distance(refined_point, connected_view_point) < SIMILARITY_THRESHOLD &&
               calculate_mahalanobis_distance(refined_point, current_view_point) < SIMILARITY_THRESHOLD) {
                connected_view_point = refined_point;
                measurement_used[proj_v][proj_u] = true;
            }
        }
    }
}

void CView::insert_into_point_cloud(std::vector<CPoint::const_ptr> &point_cloud) {
    for(int v = 0; v < IMAGE_HEIGHT; v++) {
        for(int u = 0; u < IMAGE_WIDTH; u++) {
            if(has_measurement(u, v)) {
                point_cloud.push_back(original_pointmap[v][u]);
            }
        }
    }
}

void CView::project(const CPoint &global_point, int &proj_u, int &proj_v, float &proj_depth) const {
    cv::Matx41f global_pos(global_point.pos(0),
                           global_point.pos(1),
                           global_point.pos(2),
                           1);
    cv::Matx41f local_pos = inv_transformation * global_pos;
    to_image_plane(local_pos, proj_u, proj_v);

    if(proj_u < 0 || proj_u >= IMAGE_WIDTH ||
       proj_v < 0 || proj_v >= IMAGE_HEIGHT) {
        // The point projects outside the image plane.
        proj_depth = -1;
    } else {
        proj_depth = local_pos(2);
    }
}

void CView::merge(boost::ptr_vector<CView> &views,
                  unsigned int view_idx,
                  const cv::Mat view_connectivity,
                  std::vector<CPoint::ptr> &global_point_cloud) {
    // Initialize vectors.
    // A measurement is marked as used if an existing point is refined with it.
#ifndef DEBUG
    // Use a local variable instead of a member variable.
    std::vector<std::vector<bool> > measurement_used(IMAGE_HEIGHT);
#endif
    for(int v = 0; v < IMAGE_HEIGHT; v++) {
        measurement_used[v].resize(IMAGE_WIDTH);
        for(int u = 0; u < IMAGE_WIDTH; u++) {
            measurement_used[v][u] = false;
        }
    }

    // Project all connected views into the current view to find points to be refined.
    for(unsigned int connected_idx = 0; connected_idx < view_idx; connected_idx++) {
        if(!view_connectivity.at<bool>(view_idx, connected_idx)) {
            // The view is not connected. Skip it.
            continue;
        }

        CView &connected_view = views[connected_idx];

        refine_points(connected_view, measurement_used);
    }

    // Add all the points that weren't used to refine existing points to the point cloud.
    for(int v = 0; v < IMAGE_HEIGHT; v++) {
        for(int u = 0; u < IMAGE_WIDTH; u++) {
            if(has_measurement(u, v)) {
                if(!measurement_used[v][u]) {
                    pointmap[v][u] = original_pointmap[v][u];
                    global_point_cloud.push_back(pointmap[v][u]);
                    num_added++;
                }
            } else {
                num_missing++;
            }
        }
    }
}

}
