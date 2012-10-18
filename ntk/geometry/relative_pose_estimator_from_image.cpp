#include "relative_pose_estimator_from_image.h"

#include "relative_pose_estimator_rgbd_icp.h"

#include <ntk/mesh/pcl_utils.h>
#include <ntk/mesh/mesh_generator.h>
#include <ntk/utils/time.h>

using cv::Vec3f;
using cv::Point3f;

// #define HEAVY_DEBUG

namespace ntk
{

bool RelativePoseEstimatorFromRgbFeatures::
estimateNewPose(Pose3D& new_pose,
                const RGBDImage& image,
                const FeatureSet& image_features,
                const std::vector<cv::DMatch>& matches)
{
    const float err_threshold = 0.005f;

    std::vector<Point3f> ref_points;
    std::vector<Point3f> img_points;

    foreach_idx(i, matches)
    {
        const cv::DMatch& m = matches[i];
        const FeaturePoint& ref_loc = m_target_features->locations()[m.trainIdx];
        const FeaturePoint& img_loc = image_features.locations()[m.queryIdx];

        ntk_assert(ref_loc.depth > 0, "Match without depth, should not appear");

        cv::Point3f img3d (img_loc.pt.x,
                           img_loc.pt.y,
                           img_loc.depth);

        ref_points.push_back(ref_loc.p3d);
        img_points.push_back(img3d);
    }

    ntk_dbg_print(ref_points.size(), 2);
    if (ref_points.size() < 10)
    {
        ntk_dbg(2) << "Not enough matches with depth";
        return false;
    }

    std::vector<bool> valid_points;
    double error = rms_optimize_ransac(new_pose, ref_points, img_points, valid_points, false /* use_depth */);
    error /= ref_points.size();

    ntk_dbg_print(error, 1);
    ntk_dbg_print(new_pose, 2);

    if (error > err_threshold)
        return false;

    if (m_postprocess_with_rgbd_icp)
        optimizeWithRGBDICP(new_pose, image, ref_points, img_points, valid_points);

    return true;
}

void RelativePoseEstimatorFromRgbFeatures::
optimizeWithRGBDICP(Pose3D& new_pose, const RGBDImage& image, std::vector<Point3f>& ref_points, std::vector<Point3f>& img_points, std::vector<bool>& valid_points)
{
    new_pose.toLeftCamera(image.calibration()->depth_intrinsics,
                          image.calibration()->R, image.calibration()->T);

    std::vector<Point3f> clean_ref_points;
    std::vector<Point3f> clean_img_points;
    foreach_idx(i, valid_points)
    {
        if (!valid_points[i])
            continue;
        clean_ref_points.push_back(ref_points[i]);
        clean_img_points.push_back(img_points[i]);
    }

    RGBDImage filtered_source_image;
    m_source_image->copyTo(filtered_source_image);

    RGBDImage filtered_target_image;
    m_target_image->copyTo(filtered_target_image);

    OpenniRGBDProcessor processor;
    // processor.bilateralFilter(filtered_source_image);
    // processor.bilateralFilter(filtered_target_image);
    processor.computeNormals(filtered_source_image);
    processor.computeNormals(filtered_target_image);

    MeshGenerator generator;
    generator.setMeshType(MeshGenerator::TriangleMesh);
    generator.setUseColor(true);

    generator.generate(filtered_source_image);
    Mesh source_mesh = generator.mesh();
    source_mesh.computeNormalsFromFaces();

    generator.generate(filtered_target_image);
    Mesh target_mesh = generator.mesh();
    target_mesh.computeNormalsFromFaces();

    pcl::PointCloud<pcl::PointNormal>::Ptr sampled_source_cloud;
    sampled_source_cloud.reset(new pcl::PointCloud<pcl::PointNormal>());

    pcl::PointCloud<pcl::PointNormal>::Ptr source_cloud;
    source_cloud.reset(new pcl::PointCloud<pcl::PointNormal>());

    pcl::PointCloud<pcl::PointNormal>::Ptr target_cloud;
    target_cloud.reset(new pcl::PointCloud<pcl::PointNormal>());

    // rgbdImageToPointCloud(*source_cloud, filtered_source_image);
    // rgbdImageToPointCloud(*target_cloud, filtered_target_image);
    meshToPointCloud(*source_cloud, source_mesh);
    meshToPointCloud(*target_cloud, target_mesh);

    const int num_samples = 1000;
    NormalCloudSampler<pcl::PointNormal> sampler;
    sampler.subsample(*source_cloud, *sampled_source_cloud, num_samples);

    RelativePoseEstimatorRGBDICP<pcl::PointNormal> icp_estimator;
    // RelativePoseEstimatorICPWithNormals<pcl::PointNormal> icp_estimator;
    icp_estimator.setVoxelSize(0.001);
    icp_estimator.setDistanceThreshold(0.05);
    icp_estimator.setRANSACOutlierRejectionThreshold(0.05);
    icp_estimator.setMaxIterations(100);

    icp_estimator.setSourceCloud(sampled_source_cloud);
    icp_estimator.setTargetCloud(target_cloud);

    icp_estimator.setColorFeatures(new_pose, clean_ref_points, clean_img_points);
    icp_estimator.setTargetPose(m_target_pose);
    icp_estimator.setInitialSourcePoseEstimate(new_pose);

    bool ok = icp_estimator.estimateNewPose();
    if (!ok)
    {
        ntk_dbg(1) << "RGBD-ICP failed";
        return;
    }

    new_pose = icp_estimator.estimatedSourcePose();

    new_pose.toRightCamera(image.calibration()->depth_intrinsics,
                           image.calibration()->R, image.calibration()->T);
}

bool RelativePoseEstimatorFromRgbFeatures::estimateNewPose()
{
    ntk_assert(m_source_image, "You must call setSourceImage before!");
    ntk_assert(m_target_image, "You must call setTargetImage before!");
    const RGBDImage& image = *m_source_image;

    ntk_ensure(image.mappedDepth().data, "Image must have depth mapping.");

    ntk::TimeCount tc("RelativePoseEstimator", 1);

    if (m_target_features->locations().size() < 1)
        computeTargetFeatures();

    if (m_source_features->locations().size() < 1)
    {
        m_source_features->extractFromImage(image, m_feature_parameters);
        tc.elapsedMsecs(" -- extract features from Image -- ");
    }

    m_num_matches = 0;

    FeatureSet& image_features = *m_source_features;

    std::vector<cv::DMatch> matches;
    m_target_features->matchWith(image_features, matches, 0.8f*0.8f);
    tc.elapsedMsecs(" -- match features -- ");
    ntk_dbg_print(matches.size(), 1);

#ifdef HEAVY_DEBUG
    cv::Mat3b debug_img;
    m_target_features.drawMatches(m_target_image->rgb(), image.rgb(), image_features, matches, debug_img);
    imwrite("/tmp/debug_matches.png", debug_img);
#endif

    m_num_matches = matches.size();

    if (matches.size() < m_min_matches)
        return false;

    m_estimated_pose = m_target_pose;
    m_estimated_pose.toRightCamera(image.calibration()->rgb_intrinsics,
                                   image.calibration()->R, image.calibration()->T);

    // Estimate the relative pose w.r.t the closest view.
    if (!estimateNewPose(m_estimated_pose, image, image_features, matches))
        return false;

    m_estimated_pose.toLeftCamera(image.calibration()->depth_intrinsics,
                                  image.calibration()->R, image.calibration()->T);

    return true;
}

void RelativePoseEstimatorFromRgbFeatures::resetTarget()
{
    m_target_features = toPtr(new FeatureSet);
    m_target_image = 0;
    m_estimated_pose = Pose3D();
}

void RelativePoseEstimatorFromRgbFeatures::setTargetPose(const Pose3D& pose)
{
    m_target_pose = pose;
    m_target_features = toPtr(new FeatureSet);
}

void RelativePoseEstimatorFromRgbFeatures::setTargetImage(const RGBDImage &image)
{
    ntk_ensure(image.calibration(), "Image must be calibrated.");
    m_target_image = &image;
    if (!m_target_pose.isValid())
    {
        m_target_pose = *m_target_image->calibration()->depth_pose;
    }
    m_target_features = toPtr(new FeatureSet);
}

void RelativePoseEstimatorFromRgbFeatures::setSourceImage(const RGBDImage &image)
{
    ntk_ensure(image.calibration(), "Image must be calibrated.");
    super::setSourceImage(image);
    m_source_features = toPtr(new FeatureSet);
}

void RelativePoseEstimatorFromRgbFeatures::computeTargetFeatures()
{
    m_target_features->extractFromImage(*m_target_image, m_feature_parameters);

    Pose3D rgb_pose = m_target_pose;
    rgb_pose.toRightCamera(m_target_image->calibration()->rgb_intrinsics,
                           m_target_image->calibration()->R, m_target_image->calibration()->T);
    m_target_features->compute3dLocation(rgb_pose);
}

void RelativePoseEstimatorFromRgbFeatures::setSourceImage(const RGBDImage &image, ntk::Ptr<FeatureSet> features)
{
    ntk_ensure(image.calibration(), "Image must be calibrated.");
    super::setSourceImage(image);
    m_source_features = features;
}

void RelativePoseEstimatorFromRgbFeatures::setTargetImage(const RGBDImage &image, ntk::Ptr<FeatureSet> features)
{
    setTargetImage(image);
    m_target_features = features;
}

} // ntk
